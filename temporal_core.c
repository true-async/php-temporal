/*
  +----------------------------------------------------------------------+
  | php-temporal — native async Temporal client for PHP TrueAsync        |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License").       |
  +----------------------------------------------------------------------+

  Zend-free wrapper over the Temporal Rust core c-bridge. This is the only
  translation unit that includes the bridge header (see temporal_core.h).
*/

#include <string.h>
#include <stdlib.h>

#include "temporal-sdk-core-c-bridge.h"
#include "temporal_core.h"

/* Build a ByteArrayRef over a C string (empty/unset when s is NULL). */
static inline TemporalCoreByteArrayRef temporal_php_ref(const char *s)
{
	TemporalCoreByteArrayRef r;
	r.data = (const uint8_t *) (s != NULL ? s : "");
	r.size = (s != NULL) ? strlen(s) : 0;
	return r;
}

static inline char *temporal_php_strdup(const char *s)
{
	return (s != NULL) ? strdup(s) : NULL;
}

/* Copy a core error ByteArray into a heap C string, or duplicate `fallback`
 * when the core gave no message. Does not free the source buffer. */
static char *temporal_php_dup_fail(const TemporalCoreByteArray *fail, const char *fallback)
{
	if (fail == NULL) {
		return temporal_php_strdup(fallback);
	}

	char *out = (char *) malloc(fail->size + 1);
	if (out != NULL) {
		memcpy(out, fail->data, fail->size);
		out[fail->size] = '\0';
	}
	return out;
}

/* --- Runtime ----------------------------------------------------------- */

/* Process-wide runtime, cached so temporal_php_response_free can release core buffers
 * without threading the runtime back through every layer. Set in MINIT. */
static void *temporal_php_g_runtime = NULL;

void *temporal_php_runtime_new(char **err_out)
{
	TemporalCoreRuntimeOptions options;
	memset(&options, 0, sizeof(options));

	TemporalCoreRuntimeOrFail result = temporal_core_runtime_new(&options);

	if (result.runtime == NULL) {
		if (err_out != NULL) {
			*err_out = temporal_php_dup_fail(result.fail, "failed to create Temporal core runtime");
		}
		/* The fail buffer cannot be freed through a runtime that failed to
		 * create; with default options this path is effectively unreachable. */
		return NULL;
	}

	temporal_php_g_runtime = result.runtime;
	return result.runtime;
}

void temporal_php_response_free(void *response_owner)
{
	if (response_owner != NULL) {
		temporal_core_byte_array_free((TemporalCoreRuntime *) temporal_php_g_runtime,
		                              (const TemporalCoreByteArray *) response_owner);
	}
}

void temporal_php_runtime_free(void *runtime)
{
	if (runtime != NULL) {
		temporal_core_runtime_free((TemporalCoreRuntime *) runtime);
	}
}

/* --- Client connect ---------------------------------------------------- */

/* Per-call context, owned by this TU and freed once the core invokes the
 * callback. Holds copies of every option string so the caller's buffers need
 * not outlive the async call, plus the prebuilt option structs (the bridge
 * requires options to live through the callback). */
typedef struct {
	void                 *user_data;
	temporal_php_connect_done_cb  done;
	void                 *runtime;

	char *target_url;
	char *client_name;
	char *client_version;
	char *identity;
	char *api_key;
	char *tls_root_ca;
	char *tls_client_cert;
	char *tls_client_key;
	char *tls_server_name;

	TemporalCoreClientTlsOptions    tls;
	TemporalCoreConnectionOptions   options;
} temporal_php_connect_ctx;

static void temporal_php_connect_ctx_free(temporal_php_connect_ctx *c)
{
	free(c->target_url);
	free(c->client_name);
	free(c->client_version);
	free(c->identity);
	free(c->api_key);
	free(c->tls_root_ca);
	free(c->tls_client_cert);
	free(c->tls_client_key);
	free(c->tls_server_name);
	free(c);
}

static void temporal_php_on_connect(void *user_data, TemporalCoreConnection *success,
                            const TemporalCoreByteArray *fail)
{
	temporal_php_connect_ctx *c = (temporal_php_connect_ctx *) user_data;
	char *fail_msg = NULL;

	if (success == NULL && fail != NULL) {
		fail_msg = (char *) malloc(fail->size + 1);
		if (fail_msg != NULL) {
			memcpy(fail_msg, fail->data, fail->size);
			fail_msg[fail->size] = '\0';
		}
	}

	if (fail != NULL) {
		temporal_core_byte_array_free((TemporalCoreRuntime *) c->runtime, fail);
	}

	c->done(c->user_data, (void *) success, fail_msg);
	temporal_php_connect_ctx_free(c);
}

void temporal_php_connect(void *runtime, const temporal_php_connect_params_t *params,
                  void *user_data, temporal_php_connect_done_cb done)
{
	temporal_php_connect_ctx *c = (temporal_php_connect_ctx *) calloc(1, sizeof(*c));
	if (c == NULL) {
		done(user_data, NULL, strdup("temporal: out of memory"));
		return;
	}

	c->user_data = user_data;
	c->done = done;
	c->runtime = runtime;

	c->target_url      = temporal_php_strdup(params->target_url);
	c->client_name     = temporal_php_strdup(params->client_name);
	c->client_version  = temporal_php_strdup(params->client_version);
	c->identity        = temporal_php_strdup(params->identity);
	c->api_key         = temporal_php_strdup(params->api_key);
	c->tls_root_ca     = temporal_php_strdup(params->tls_server_root_ca_cert);
	c->tls_client_cert = temporal_php_strdup(params->tls_client_cert);
	c->tls_client_key  = temporal_php_strdup(params->tls_client_private_key);
	c->tls_server_name = temporal_php_strdup(params->tls_server_name);

	memset(&c->options, 0, sizeof(c->options));
	c->options.target_url     = temporal_php_ref(c->target_url);
	c->options.client_name    = temporal_php_ref(c->client_name);
	c->options.client_version = temporal_php_ref(c->client_version);
	c->options.identity       = temporal_php_ref(c->identity);
	c->options.api_key        = temporal_php_ref(c->api_key);

	if (params->tls_enabled) {
		memset(&c->tls, 0, sizeof(c->tls));
		c->tls.server_root_ca_cert = temporal_php_ref(c->tls_root_ca);
		c->tls.client_cert         = temporal_php_ref(c->tls_client_cert);
		c->tls.client_private_key  = temporal_php_ref(c->tls_client_key);
		c->tls.domain              = temporal_php_ref(c->tls_server_name);
		c->options.tls_options = &c->tls;
	}

	temporal_core_client_connect((TemporalCoreRuntime *) runtime, &c->options, c,
	                             temporal_php_on_connect);
}

void temporal_php_connection_free(void *connection)
{
	if (connection != NULL) {
		temporal_core_client_free((TemporalCoreConnection *) connection);
	}
}

/* --- Async RPC --------------------------------------------------------- */

typedef struct {
	void              *user_data;
	temporal_php_rpc_done_cb   done;
	void              *runtime;
	char              *method;   /* copied */
	uint8_t           *req;      /* copied */
	size_t             req_len;
	char             **meta_strings;  /* copied "<key>\n<value>" entries */
	size_t             meta_count;
	TemporalCoreByteArrayRef *meta_refs;
	TemporalCoreRpcCallOptions options;
} temporal_php_rpc_ctx;

static void temporal_php_rpc_ctx_free(temporal_php_rpc_ctx *c)
{
	for (size_t i = 0; i < c->meta_count; i++) {
		free(c->meta_strings[i]);
	}
	free(c->meta_strings);
	free(c->meta_refs);
	free(c->method);
	free(c->req);
	free(c);
}

/* Copy a (small) core ByteArray into a heap C string; frees the core buffer. */
static char *temporal_php_copy_str(void *runtime, const TemporalCoreByteArray *src, size_t *len_out)
{
	char *out = NULL;
	size_t len = 0;

	if (src != NULL) {
		len = src->size;
		out = (char *) malloc(len + 1);
		if (out != NULL) {
			memcpy(out, src->data, len);
			out[len] = 0;
		}
		temporal_core_byte_array_free((TemporalCoreRuntime *) runtime, src);
	}

	*len_out = (out != NULL) ? len : 0;
	return out;
}

static void temporal_php_on_rpc(void *user_data, const TemporalCoreByteArray *success,
                        uint32_t status_code, const TemporalCoreByteArray *failure_message,
                        const TemporalCoreByteArray *failure_details)
{
	temporal_php_rpc_ctx *c = (temporal_php_rpc_ctx *) user_data;

	/* Success bytes are handed up WITHOUT a copy: the core buffer itself is the
	 * owner, released later by the reactor via temporal_php_response_free(). */
	const uint8_t *ok = NULL;
	size_t ok_len = 0;
	void *ok_owner = NULL;
	if (success != NULL) {
		ok = success->data;
		ok_len = success->size;
		ok_owner = (void *) success;
	}

	/* Failure message + serialized status details are small and rare: copy +
	 * free now (only the large success payload is zero-copied). */
	size_t fail_len = 0;
	char *fail = temporal_php_copy_str(c->runtime, failure_message, &fail_len);
	size_t details_len = 0;
	char *details = temporal_php_copy_str(c->runtime, failure_details, &details_len);

	c->done(c->user_data, ok, ok_len, ok_owner, status_code, fail, fail_len,
	        (uint8_t *) details, details_len);
	temporal_php_rpc_ctx_free(c);
}

void temporal_php_rpc_call(void *runtime, void *connection, int service,
                   const char *method, const uint8_t *req, size_t req_len,
                   uint32_t timeout_ms,
                   const char *const *metadata, size_t metadata_count,
                   void *cancel_token,
                   void *user_data, temporal_php_rpc_done_cb done)
{
	temporal_php_rpc_ctx *c = (temporal_php_rpc_ctx *) calloc(1, sizeof(*c));
	if (c == NULL) {
		done(user_data, NULL, 0, NULL, 0, strdup("temporal: out of memory"), 0, NULL, 0);
		return;
	}

	c->user_data = user_data;
	c->done = done;
	c->runtime = runtime;

	c->method = strdup(method != NULL ? method : "");
	c->req_len = req_len;
	c->req = (uint8_t *) malloc(req_len > 0 ? req_len : 1);
	if (c->method == NULL || c->req == NULL) {
		temporal_php_rpc_ctx_free(c);
		done(user_data, NULL, 0, NULL, 0, strdup("temporal: out of memory"), 0, NULL, 0);
		return;
	}
	if (req_len > 0 && req != NULL) {
		memcpy(c->req, req, req_len);
	}

	memset(&c->options, 0, sizeof(c->options));
	c->options.service = (enum TemporalCoreRpcService) service;
	c->options.rpc.data = (const uint8_t *) c->method;
	c->options.rpc.size = strlen(c->method);
	c->options.req.data = c->req;
	c->options.req.size = c->req_len;
	/* Single attempt: retries are owned by the SDK layer (BaseClient::call),
	 * which respects the per-call RetryOptions. Avoids double retry budgets. */
	c->options.retry = false;
	c->options.timeout_millis = timeout_ms;
	c->options.cancellation_token = (const TemporalCoreCancellationToken *) cancel_token;

	if (metadata_count > 0 && metadata != NULL) {
		c->meta_count = metadata_count;
		c->meta_strings = (char **) calloc(metadata_count, sizeof(char *));
		c->meta_refs = (TemporalCoreByteArrayRef *) calloc(metadata_count, sizeof(TemporalCoreByteArrayRef));
		for (size_t i = 0; i < metadata_count; i++) {
			c->meta_strings[i] = strdup(metadata[i] != NULL ? metadata[i] : "");
			c->meta_refs[i].data = (const uint8_t *) c->meta_strings[i];
			c->meta_refs[i].size = strlen(c->meta_strings[i]);
		}
		c->options.metadata.data = c->meta_refs;
		c->options.metadata.size = metadata_count;
	}

	temporal_core_client_rpc_call((TemporalCoreConnection *) connection, &c->options, c,
	                              temporal_php_on_rpc);
}

void *temporal_php_cancel_token_new(void)
{
	return temporal_core_cancellation_token_new();
}

void temporal_php_cancel_token_cancel(void *token)
{
	if (token != NULL) {
		temporal_core_cancellation_token_cancel((TemporalCoreCancellationToken *) token);
	}
}

void temporal_php_cancel_token_free(void *token)
{
	if (token != NULL) {
		temporal_core_cancellation_token_free((TemporalCoreCancellationToken *) token);
	}
}

/* --- Worker ------------------------------------------------------------ */

void *temporal_php_worker_new(void *connection, const char *ns, const char *task_queue,
                      const temporal_php_worker_options_t *options, char **err_out)
{
	TemporalCoreWorkerOptions opt;
	memset(&opt, 0, sizeof(opt));

	opt.namespace_ = temporal_php_ref(ns);
	opt.task_queue = temporal_php_ref(task_queue);

	/* No versioning. */
	opt.versioning_strategy.tag = None;
	opt.versioning_strategy.none.build_id = temporal_php_ref("");

	/* Fixed-size slot suppliers. All four must be valid. */
	opt.tuner.activity_slot_supplier.tag = FixedSize;
	opt.tuner.activity_slot_supplier.fixed_size.num_slots = options->activity_slots;
	opt.tuner.workflow_slot_supplier.tag = FixedSize;
	opt.tuner.workflow_slot_supplier.fixed_size.num_slots = options->workflow_slots;
	opt.tuner.local_activity_slot_supplier.tag = FixedSize;
	opt.tuner.local_activity_slot_supplier.fixed_size.num_slots = options->local_activity_slots;
	opt.tuner.nexus_task_slot_supplier.tag = FixedSize;
	opt.tuner.nexus_task_slot_supplier.fixed_size.num_slots = options->nexus_slots;

	/* Handle workflow, activity and local-activity tasks (a Temporal worker does
	 * all three). Local activities run in-process and surface on the same activity
	 * poll (Start.is_local); without this flag the core never dispatches them and a
	 * workflow that schedules one hangs until its task times out. */
	opt.task_types.enable_workflows = true;
	opt.task_types.enable_remote_activities = true;
	opt.task_types.enable_local_activities = true;

	/* Sticky execution: keep workflow runs cached between tasks so a fired timer
	 * or resolved activity resumes the live instance instead of replaying from
	 * scratch. This is purely a performance optimization: the codec correlates
	 * commands by a deterministic per-run seq, so replay (eviction, restart, cache
	 * pressure) stays correct regardless of cache size. */
	opt.max_cached_workflows = options->max_cached_workflows;

	opt.sticky_queue_schedule_to_start_timeout_millis = options->sticky_schedule_to_start_ms;
	opt.graceful_shutdown_period_millis = options->graceful_shutdown_ms;
	opt.max_heartbeat_throttle_interval_millis = 60000;
	opt.default_heartbeat_throttle_interval_millis = 30000;
	opt.nonsticky_to_sticky_poll_ratio = 0.2f;

	/* Poller behaviors (simple maximum). These pointers may stay on the stack:
	 * temporal_core_worker_new consumes the options synchronously. */
	TemporalCorePollerBehaviorSimpleMaximum act_poll;
	act_poll.simple_maximum = options->activity_pollers;
	TemporalCorePollerBehaviorSimpleMaximum wf_poll;
	wf_poll.simple_maximum = options->workflow_pollers;
	TemporalCorePollerBehaviorSimpleMaximum nx_poll;
	nx_poll.simple_maximum = options->nexus_pollers;
	opt.activity_task_poller_behavior.simple_maximum = &act_poll;
	opt.workflow_task_poller_behavior.simple_maximum = &wf_poll;
	opt.nexus_task_poller_behavior.simple_maximum = &nx_poll;

	TemporalCoreWorkerOrFail result =
		temporal_core_worker_new((TemporalCoreConnection *) connection, &opt);

	if (result.worker == NULL) {
		if (err_out != NULL) {
			*err_out = temporal_php_dup_fail(result.fail, "failed to create Temporal worker");
		}
		/* Unlike runtime_new, a live runtime exists here to free the buffer
		 * through — and we free it regardless of err_out (no leak when NULL). */
		if (result.fail != NULL) {
			temporal_core_byte_array_free((TemporalCoreRuntime *) temporal_php_g_runtime, result.fail);
		}
		return NULL;
	}

	return result.worker;
}

void temporal_php_worker_free(void *worker)
{
	if (worker != NULL) {
		temporal_core_worker_free((TemporalCoreWorker *) worker);
	}
}

void temporal_php_worker_initiate_shutdown(void *worker)
{
	if (worker != NULL) {
		temporal_core_worker_initiate_shutdown((TemporalCoreWorker *) worker);
	}
}

/* poll ctx: carries the Zend-side call struct + the delivery callback. */
typedef struct {
	void                *user_data;
	temporal_php_worker_poll_cb  done;
} temporal_php_poll_ctx;

static void temporal_php_on_worker_poll(void *user_data, const TemporalCoreByteArray *success,
                                const TemporalCoreByteArray *fail)
{
	temporal_php_poll_ctx *c = (temporal_php_poll_ctx *) user_data;

	/* Task bytes are handed up zero-copy (the core buffer is the owner). */
	const uint8_t *task = NULL;
	size_t task_len = 0;
	void *task_owner = NULL;
	if (success != NULL) {
		task = success->data;
		task_len = success->size;
		task_owner = (void *) success;
	}

	size_t fail_len = 0;
	char *failmsg = temporal_php_copy_str(temporal_php_g_runtime, fail, &fail_len);

	/* success==NULL && fail==NULL means a shutdown poll. */
	c->done(c->user_data, task, task_len, task_owner, failmsg, fail_len);
	free(c);
}

void temporal_php_worker_poll_activity(void *worker, void *user_data, temporal_php_worker_poll_cb done)
{
	temporal_php_poll_ctx *c = (temporal_php_poll_ctx *) calloc(1, sizeof(*c));
	if (c == NULL) {
		done(user_data, NULL, 0, NULL, strdup("temporal: out of memory"), 0);
		return;
	}
	c->user_data = user_data;
	c->done = done;

	temporal_core_worker_poll_activity_task((TemporalCoreWorker *) worker, c, temporal_php_on_worker_poll);
}

/* done ctx: carries the call struct + callback (+ a completion copy, if any). */
typedef struct {
	void                *user_data;
	temporal_php_worker_done_cb  done;
	uint8_t             *completion;   /* copied; NULL for shutdown finalize */
} temporal_php_worker_done_ctx;

static void temporal_php_on_worker_done(void *user_data, const TemporalCoreByteArray *fail)
{
	temporal_php_worker_done_ctx *c = (temporal_php_worker_done_ctx *) user_data;

	size_t fail_len = 0;
	char *failmsg = temporal_php_copy_str(temporal_php_g_runtime, fail, &fail_len);

	c->done(c->user_data, failmsg, fail_len);
	free(c->completion);
	free(c);
}

void temporal_php_worker_complete_activity(void *worker, const uint8_t *completion, size_t len,
                                   void *user_data, temporal_php_worker_done_cb done)
{
	temporal_php_worker_done_ctx *c = (temporal_php_worker_done_ctx *) calloc(1, sizeof(*c));
	if (c != NULL) {
		c->completion = (uint8_t *) malloc(len > 0 ? len : 1);
	}
	if (c == NULL || c->completion == NULL) {
		free(c);
		/* Under double-OOM the strdup may yield NULL = success; accepted. */
		done(user_data, strdup("temporal: out of memory"), 0);
		return;
	}
	c->user_data = user_data;
	c->done = done;
	if (len > 0 && completion != NULL) {
		memcpy(c->completion, completion, len);
	}

	TemporalCoreByteArrayRef ref;
	ref.data = c->completion;
	ref.size = len;

	temporal_core_worker_complete_activity_task((TemporalCoreWorker *) worker, ref, c,
	                                            temporal_php_on_worker_done);
}

/* Workflow poll/complete reuse the generic poll/done callbacks above; only the
 * underlying bridge call differs. */
void temporal_php_worker_poll_workflow(void *worker, void *user_data, temporal_php_worker_poll_cb done)
{
	temporal_php_poll_ctx *c = (temporal_php_poll_ctx *) calloc(1, sizeof(*c));
	if (c == NULL) {
		done(user_data, NULL, 0, NULL, strdup("temporal: out of memory"), 0);
		return;
	}
	c->user_data = user_data;
	c->done = done;

	temporal_core_worker_poll_workflow_activation((TemporalCoreWorker *) worker, c, temporal_php_on_worker_poll);
}

void temporal_php_worker_complete_workflow(void *worker, const uint8_t *completion, size_t len,
                                   void *user_data, temporal_php_worker_done_cb done)
{
	temporal_php_worker_done_ctx *c = (temporal_php_worker_done_ctx *) calloc(1, sizeof(*c));
	if (c != NULL) {
		c->completion = (uint8_t *) malloc(len > 0 ? len : 1);
	}
	if (c == NULL || c->completion == NULL) {
		free(c);
		done(user_data, strdup("temporal: out of memory"), 0);
		return;
	}
	c->user_data = user_data;
	c->done = done;
	if (len > 0 && completion != NULL) {
		memcpy(c->completion, completion, len);
	}

	TemporalCoreByteArrayRef ref;
	ref.data = c->completion;
	ref.size = len;

	temporal_core_worker_complete_workflow_activation((TemporalCoreWorker *) worker, ref, c,
	                                                  temporal_php_on_worker_done);
}

void temporal_php_worker_finalize_shutdown(void *worker, void *user_data, temporal_php_worker_done_cb done)
{
	temporal_php_worker_done_ctx *c = (temporal_php_worker_done_ctx *) calloc(1, sizeof(*c));
	if (c == NULL) {
		done(user_data, strdup("temporal: out of memory"), 0);
		return;
	}
	c->user_data = user_data;
	c->done = done;

	temporal_core_worker_finalize_shutdown((TemporalCoreWorker *) worker, c, temporal_php_on_worker_done);
}

char *temporal_php_worker_record_heartbeat(void *worker, const uint8_t *heartbeat, size_t len)
{
	TemporalCoreByteArrayRef ref;
	ref.data = heartbeat;
	ref.size = len;

	const TemporalCoreByteArray *fail =
		temporal_core_worker_record_activity_heartbeat((TemporalCoreWorker *) worker, ref);

	size_t fail_len = 0;
	return temporal_php_copy_str(temporal_php_g_runtime, fail, &fail_len);
}
