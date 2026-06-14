/*
  +----------------------------------------------------------------------+
  | php-temporal — native async Temporal transport for PHP TrueAsync     |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License").       |
  +----------------------------------------------------------------------+

  Zend-facing transport: the Core\Connection object and the cross-thread call
  bridge. Talks to the core only through temporal_core.h.

  Threading: the core runs the gRPC on its own Tokio threads. A call parks the
  current coroutine, and the core's completion callback (on a Tokio thread)
  writes the result and fires a trigger (uv_async_send, the one thread-safe
  libuv primitive). The reactor then resumes the coroutine, which reads the
  result. The Tokio-side callback never touches Zend/TSRM; the only shared state
  is a mutex-guarded, refcounted call struct.
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <unistd.h>

#include "php.h"
#include "zend_exceptions.h"
#include "zend_async_API.h"

#include "php_temporal.h"
#include "temporal_internal.h"
#include "temporal_core.h"

/* When a suspending Core method is called outside any coroutine (the top-level
 * "main" flow), launch the scheduler — it converts the current execution into
 * the main coroutine — so the call works without an explicit Async\spawn()
 * wrapper, exactly as Async\await() / Async\delay() do (ext/async's
 * SCHEDULER_LAUNCH). Only the launch itself failing (e.g. the reactor is
 * disabled) throws and returns. */
#define TEMPORAL_ENSURE_COROUTINE() \
	do { \
		if (UNEXPECTED(ZEND_ASYNC_CURRENT_COROUTINE == NULL)) { \
			if (!ZEND_ASYNC_SCHEDULER_LAUNCH()) { \
				RETURN_THROWS(); \
			} \
		} \
	} while (0)

/* ====================================================================== */
/* Refcounted core-handle owner                                           */
/* ====================================================================== */

/* Owns one core handle (the Rust Connection/Worker Box). The zend object holds
 * one ref; each in-flight call adds one. Whoever drops the last ref frees the
 * Box via free_box, on either thread — it touches only C/Rust memory, never
 * Zend. This honors the bridge contract ("must live through callback"): an op
 * keeps the handle alive until its completion callback fires, even when the
 * awaiting coroutine was cancelled and the zend object already destroyed. */
typedef struct {
	MUTEX_T  mutex;
	int      refcount;
	void    *box;
	void   (*free_box)(void *);
} temporal_php_handle_t;

/* Returns NULL on allocation failure; the caller throws and frees the box. */
static temporal_php_handle_t *temporal_php_handle_new(void *box, void (*free_box)(void *))
{
	temporal_php_handle_t *h = (temporal_php_handle_t *) calloc(1, sizeof(*h));
	if (h == NULL) {
		return NULL;
	}
	h->mutex = tsrm_mutex_alloc();
	h->refcount = 1;
	h->box = box;
	h->free_box = free_box;
	return h;
}

static void temporal_php_handle_addref(temporal_php_handle_t *h)
{
	tsrm_mutex_lock(h->mutex);
	h->refcount++;
	tsrm_mutex_unlock(h->mutex);
}

static void temporal_php_handle_release(temporal_php_handle_t *h)
{
	tsrm_mutex_lock(h->mutex);
	bool last = (--h->refcount == 0);
	tsrm_mutex_unlock(h->mutex);

	if (last) {
		h->free_box(h->box);
		tsrm_mutex_free(h->mutex);
		free(h);
	}
}

/* ====================================================================== */
/* Core\Connection object lifecycle                                       */
/* ====================================================================== */

static zend_object_handlers temporal_connection_handlers;

static zend_object *temporal_connection_create_object(zend_class_entry *ce)
{
	temporal_connection_obj *obj = zend_object_alloc(sizeof(temporal_connection_obj), ce);

	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &temporal_connection_handlers;
	obj->connection = NULL;

	return &obj->std;
}

static void temporal_connection_free_object(zend_object *object)
{
	temporal_connection_obj *obj = temporal_connection_from_obj(object);

	if (obj->connection != NULL) {
		temporal_php_handle_release((temporal_php_handle_t *) obj->connection);
		obj->connection = NULL;
	}

	zend_object_std_dtor(&obj->std);
}

static zend_object_handlers temporal_worker_handlers;

static zend_object *temporal_worker_create_object(zend_class_entry *ce)
{
	temporal_worker_obj *obj = zend_object_alloc(sizeof(temporal_worker_obj), ce);

	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &temporal_worker_handlers;
	obj->worker = NULL;
	obj->connection = NULL;
	obj->finalized = false;

	return &obj->std;
}

static void temporal_worker_free_object(zend_object *object)
{
	temporal_worker_obj *obj = temporal_worker_from_obj(object);

	if (obj->worker != NULL) {
		temporal_php_handle_release((temporal_php_handle_t *) obj->worker);
		obj->worker = NULL;
	}
	if (obj->connection != NULL) {
		OBJ_RELEASE(obj->connection);
		obj->connection = NULL;
	}

	zend_object_std_dtor(&obj->std);
}

void temporal_register_objects(void)
{
	memcpy(&temporal_connection_handlers, &std_object_handlers,
	       sizeof(zend_object_handlers));
	temporal_connection_handlers.offset = offsetof(temporal_connection_obj, std);
	temporal_connection_handlers.free_obj = temporal_connection_free_object;
	temporal_ce_connection->create_object = temporal_connection_create_object;

	memcpy(&temporal_worker_handlers, &std_object_handlers,
	       sizeof(zend_object_handlers));
	temporal_worker_handlers.offset = offsetof(temporal_worker_obj, std);
	temporal_worker_handlers.free_obj = temporal_worker_free_object;
	temporal_ce_worker->create_object = temporal_worker_create_object;
}

/* ====================================================================== */
/* Cross-thread call bridge                                               */
/* ====================================================================== */

/* The result of one async op, written once by the core (Tokio) thread and read
 * once by the reactor. Defined here so it can be embedded in the shared call
 * struct AND handed back to the caller without duplicating the field list.
 * Only the relevant fields are set: `connection` for connect; `data`/`owner`
 * for an rpc reply; `fail`/`status` on failure. `data` is borrowed from the
 * core-owned `owner` (released with temporal_php_response_free); `fail` is heap. */
typedef struct {
	void          *connection;
	const uint8_t *data;
	size_t         data_len;
	void          *owner;
	uint32_t       status;
	char          *fail;
	uint8_t       *details;       /* serialized google.rpc.Status (heap), or NULL */
	size_t         details_len;
} temporal_result_t;

typedef struct {
	bool              cancelled;
	temporal_result_t r;
} temporal_outcome_t;

/* Shared between the reactor and a Tokio thread; refcounted (awaiter +
 * in-flight). The Tokio side touches only this (a TSRM mutex + uv_async_send),
 * never Zend/TSRM. */
typedef struct {
	MUTEX_T  mutex;
	int      refcount;
	bool     completed;
	zend_async_trigger_event_t *trigger;   /* NULL once the reactor tears down */
	temporal_php_handle_t *handle;                 /* core handle kept alive for this op (NULL for connect) */
	void *cancel_token;                    /* reactor-owned RPC cancel token (NULL otherwise) */
	temporal_result_t result;
} temporal_call_t;

static void temporal_call_free(temporal_call_t *call)
{
	if (call->handle != NULL) {
		temporal_php_handle_release(call->handle);
	}
	tsrm_mutex_free(call->mutex);
	free(call);
}

/* Release a delivered-but-unconsumed result (cancellation / orphan paths). */
static void temporal_result_drop(const temporal_result_t *r)
{
	if (r->connection != NULL) {
		temporal_php_connection_free(r->connection);
	}
	if (r->owner != NULL) {
		temporal_php_response_free(r->owner);
	}
	free(r->fail);
	free(r->details);
}

/* Wake the reactor and drop the in-flight ref. Caller holds the mutex. */
static bool temporal_call_deliver_locked(temporal_call_t *call)
{
	call->completed = true;
	call->trigger->trigger(call->trigger);   /* uv_async_send */
	return (--call->refcount == 0);
}

/* connect delivery (Tokio thread). */
static void temporal_connect_done(void *user_data, void *connection, char *fail)
{
	temporal_call_t *call = (temporal_call_t *) user_data;

	tsrm_mutex_lock(call->mutex);
	if (call->trigger != NULL) {
		call->result.connection = connection;
		call->result.fail = fail;
		bool last = temporal_call_deliver_locked(call);
		tsrm_mutex_unlock(call->mutex);
		if (last) temporal_call_free(call);
	} else {
		temporal_result_t orphan = { .connection = connection, .fail = fail };
		bool last = (--call->refcount == 0);
		tsrm_mutex_unlock(call->mutex);
		temporal_result_drop(&orphan);
		if (last) temporal_call_free(call);
	}
}

/* rpc delivery (Tokio thread). */
static void temporal_rpc_done(void *user_data, const uint8_t *success, size_t success_len,
                              void *success_owner, uint32_t status_code,
                              char *fail, size_t fail_len, uint8_t *details, size_t details_len)
{
	temporal_call_t *call = (temporal_call_t *) user_data;
	(void) fail_len;

	tsrm_mutex_lock(call->mutex);
	if (call->trigger != NULL) {
		call->result.data = success;
		call->result.data_len = success_len;
		call->result.owner = success_owner;
		call->result.status = status_code;
		call->result.fail = fail;
		call->result.details = details;
		call->result.details_len = details_len;
		bool last = temporal_call_deliver_locked(call);
		tsrm_mutex_unlock(call->mutex);
		if (last) temporal_call_free(call);
	} else {
		temporal_result_t orphan = {
			.data = success, .owner = success_owner, .fail = fail, .details = details,
		};
		bool last = (--call->refcount == 0);
		tsrm_mutex_unlock(call->mutex);
		temporal_result_drop(&orphan);
		if (last) temporal_call_free(call);
	}
}

/* Construct an armed-but-unwired call: allocation only. The caller wires the
 * wakeup (resume_when + start) before launching the async op. Returns NULL with
 * a pending exception on failure. */
static temporal_call_t *temporal_call_new(void)
{
	temporal_call_t *call = (temporal_call_t *) calloc(1, sizeof(*call));
	if (call == NULL) {
		zend_throw_error(NULL, "temporal: out of memory");
		return NULL;
	}
	call->mutex = tsrm_mutex_alloc();
	call->refcount = 2;                          /* awaiter + in-flight */

	call->trigger = ZEND_ASYNC_NEW_TRIGGER_EVENT();
	if (call->trigger == NULL) {
		tsrm_mutex_free(call->mutex);
		free(call);
		zend_throw_error(NULL, "temporal: failed to create trigger event");
		return NULL;
	}

	return call;
}

/* Suspend until the call completes, then collect the outcome and release the
 * call. On cancellation out->cancelled is set and any delivered result dropped. */
static void temporal_call_collect(temporal_call_t *call, zend_coroutine_t *co,
                                  temporal_outcome_t *out)
{
	ZEND_ASYNC_SUSPEND();

	tsrm_mutex_lock(call->mutex);
	zend_async_trigger_event_t *trigger = call->trigger;
	call->trigger = NULL;                        /* a late callback drops its own result */
	bool completed = call->completed;
	void *cancel_token = call->cancel_token;     /* reactor-owned; capture before any free */
	temporal_result_t r = call->result;          /* copy the bundle out */
	memset(&call->result, 0, sizeof(call->result));
	bool last = (--call->refcount == 0);
	tsrm_mutex_unlock(call->mutex);

	zend_async_waker_clean(co);

	if (trigger != NULL) {
		trigger->base.dispose(&trigger->base);   /* dispose on the reactor thread */
	}

	out->cancelled = false;
	memset(&out->r, 0, sizeof(out->r));

	if (EG(exception) != NULL) {
		out->cancelled = true;
		/* Cancelled while the op is in flight: tell the core to abort so it
		 * returns promptly, drops the in-flight ref and frees the handle now
		 * instead of lingering until the call completes on its own. */
		if (!completed && cancel_token != NULL) {
			temporal_php_cancel_token_cancel(cancel_token);
		}
		temporal_result_drop(&r);
		if (last) temporal_call_free(call);
		temporal_php_cancel_token_free(cancel_token);
		return;
	}

	if (last) temporal_call_free(call);
	temporal_php_cancel_token_free(cancel_token);

	if (completed) {
		out->r = r;                              /* ownership passes to the caller */
	}
}

/* Park the current coroutine on a connect; fills `out`. */
static void temporal_run_connect(const temporal_php_connect_params_t *params, temporal_outcome_t *out)
{
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

	temporal_call_t *call = temporal_call_new();
	if (call == NULL) {
		out->cancelled = true;                   /* exception already pending */
		memset(&out->r, 0, sizeof(out->r));
		return;
	}

	ZEND_ASYNC_WAKER_NEW(co);
	zend_async_resume_when(co, &call->trigger->base, false,
	                       zend_async_waker_callback_resolve, NULL);
	call->trigger->base.start(&call->trigger->base);   /* keep the loop alive */

	temporal_php_connect(temporal_runtime_handle, params, call, temporal_connect_done);
	temporal_call_collect(call, co, out);
}

/* Park the current coroutine on a unary RPC; fills `out`. */
static void temporal_run_rpc(temporal_php_handle_t *handle, int service, const char *method,
                             const uint8_t *req, size_t req_len, uint32_t timeout_ms,
                             const char *const *metadata, size_t metadata_count,
                             temporal_outcome_t *out)
{
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

	temporal_call_t *call = temporal_call_new();
	if (call == NULL) {
		out->cancelled = true;
		memset(&out->r, 0, sizeof(out->r));
		return;
	}

	/* Keep the handle alive until this op's callback fires, even past a cancel. */
	call->handle = handle;
	temporal_php_handle_addref(handle);

	/* A cancel token so a cancelled coroutine aborts the RPC promptly. */
	call->cancel_token = temporal_php_cancel_token_new();

	ZEND_ASYNC_WAKER_NEW(co);
	zend_async_resume_when(co, &call->trigger->base, false,
	                       zend_async_waker_callback_resolve, NULL);
	call->trigger->base.start(&call->trigger->base);

	temporal_php_rpc_call(temporal_runtime_handle, handle->box, service, method, req, req_len,
	              timeout_ms, metadata, metadata_count, call->cancel_token, call, temporal_rpc_done);
	temporal_call_collect(call, co, out);
}

/* A metadata key or value with an embedded newline would corrupt the core's
 * "<key>\n<value>" wire form (the entry would parse as a different pair). */
static bool temporal_metadata_part_valid(const char *s, size_t len)
{
	return memchr(s, '\n', len) == NULL;
}

/* Flatten a PHP metadata map (key => string | list<string>) into the core's
 * "<key>\n<value>" wire entries. Returns the count, or (size_t)-1 with a
 * ValueError thrown when a key/value embeds a newline; *zstrs / *ptrs are
 * emalloc'd parallel arrays the caller releases (count entries on success,
 * none on failure). */
static size_t temporal_flatten_metadata(HashTable *ht, zend_string ***zstrs, const char ***ptrs)
{
	size_t cap = 0;
	zval *v;
	ZEND_HASH_FOREACH_VAL(ht, v) {
		cap += (Z_TYPE_P(v) == IS_ARRAY) ? zend_hash_num_elements(Z_ARRVAL_P(v)) : 1;
	} ZEND_HASH_FOREACH_END();

	*zstrs = NULL;
	*ptrs = NULL;

	if (cap == 0) {
		return 0;
	}

	*zstrs = (zend_string **) emalloc(cap * sizeof(zend_string *));
	*ptrs = (const char **) emalloc(cap * sizeof(char *));
	size_t n = 0;

	zend_string *key;
	ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, v) {
		if (key == NULL) {
			continue;                            /* skip non-string keys */
		}
		if (!temporal_metadata_part_valid(ZSTR_VAL(key), ZSTR_LEN(key))) {
			goto invalid;
		}
		if (Z_TYPE_P(v) == IS_ARRAY) {
			zval *item;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), item) {
				zend_string *s = zval_get_string(item);
				if (!temporal_metadata_part_valid(ZSTR_VAL(s), ZSTR_LEN(s))) {
					zend_string_release(s);
					goto invalid;
				}
				zend_string *kv = zend_strpprintf(0, "%s\n%s", ZSTR_VAL(key), ZSTR_VAL(s));
				(*zstrs)[n] = kv;
				(*ptrs)[n] = ZSTR_VAL(kv);
				n++;
				zend_string_release(s);
			} ZEND_HASH_FOREACH_END();
		} else {
			zend_string *s = zval_get_string(v);
			if (!temporal_metadata_part_valid(ZSTR_VAL(s), ZSTR_LEN(s))) {
				zend_string_release(s);
				goto invalid;
			}
			zend_string *kv = zend_strpprintf(0, "%s\n%s", ZSTR_VAL(key), ZSTR_VAL(s));
			(*zstrs)[n] = kv;
			(*ptrs)[n] = ZSTR_VAL(kv);
			n++;
			zend_string_release(s);
		}
	} ZEND_HASH_FOREACH_END();

	return n;

invalid:
	for (size_t i = 0; i < n; i++) {
		zend_string_release((*zstrs)[i]);
	}
	efree(*zstrs);
	efree(*ptrs);
	*zstrs = NULL;
	*ptrs = NULL;
	zend_value_error("Temporal metadata keys and values must not contain newlines");
	return (size_t) -1;
}

/* poll delivery (Tokio thread): a task (zero-copy), a shutdown (all NULL) or a
 * failure. */
static void temporal_worker_poll_done(void *user_data, const uint8_t *task, size_t task_len,
                                      void *task_owner, char *fail, size_t fail_len)
{
	temporal_call_t *call = (temporal_call_t *) user_data;
	(void) fail_len;

	tsrm_mutex_lock(call->mutex);
	if (call->trigger != NULL) {
		call->result.data = task;
		call->result.data_len = task_len;
		call->result.owner = task_owner;
		call->result.fail = fail;
		bool last = temporal_call_deliver_locked(call);
		tsrm_mutex_unlock(call->mutex);
		if (last) temporal_call_free(call);
	} else {
		temporal_result_t orphan = { .data = task, .owner = task_owner, .fail = fail };
		bool last = (--call->refcount == 0);
		tsrm_mutex_unlock(call->mutex);
		temporal_result_drop(&orphan);
		if (last) temporal_call_free(call);
	}
}

/* void worker op delivery (Tokio thread): success or a failure message. */
static void temporal_worker_op_done(void *user_data, char *fail, size_t fail_len)
{
	temporal_call_t *call = (temporal_call_t *) user_data;
	(void) fail_len;

	tsrm_mutex_lock(call->mutex);
	if (call->trigger != NULL) {
		call->result.fail = fail;
		bool last = temporal_call_deliver_locked(call);
		tsrm_mutex_unlock(call->mutex);
		if (last) temporal_call_free(call);
	} else {
		free(fail);
		bool last = (--call->refcount == 0);
		tsrm_mutex_unlock(call->mutex);
		if (last) temporal_call_free(call);
	}
}

/* Park the current coroutine on a worker poll (activity or workflow, per the
 * given bridge poll function); fills `out`. */
static void temporal_run_worker_poll(temporal_php_handle_t *handle,
                                     void (*poll)(void *, void *, temporal_php_worker_poll_cb),
                                     temporal_outcome_t *out)
{
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

	temporal_call_t *call = temporal_call_new();
	if (call == NULL) {
		out->cancelled = true;
		memset(&out->r, 0, sizeof(out->r));
		return;
	}

	call->handle = handle;
	temporal_php_handle_addref(handle);

	ZEND_ASYNC_WAKER_NEW(co);
	zend_async_resume_when(co, &call->trigger->base, false,
	                       zend_async_waker_callback_resolve, NULL);
	call->trigger->base.start(&call->trigger->base);

	poll(handle->box, call, temporal_worker_poll_done);
	temporal_call_collect(call, co, out);
}

/* Park the current coroutine on a worker completion (activity or workflow, per
 * the given bridge complete function); fills `out`. */
static void temporal_run_worker_complete(temporal_php_handle_t *handle,
                                         void (*complete)(void *, const uint8_t *, size_t,
                                                          void *, temporal_php_worker_done_cb),
                                         const uint8_t *completion, size_t len,
                                         temporal_outcome_t *out)
{
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

	temporal_call_t *call = temporal_call_new();
	if (call == NULL) {
		out->cancelled = true;
		memset(&out->r, 0, sizeof(out->r));
		return;
	}

	call->handle = handle;
	temporal_php_handle_addref(handle);

	ZEND_ASYNC_WAKER_NEW(co);
	zend_async_resume_when(co, &call->trigger->base, false,
	                       zend_async_waker_callback_resolve, NULL);
	call->trigger->base.start(&call->trigger->base);

	complete(handle->box, completion, len, call, temporal_worker_op_done);
	temporal_call_collect(call, co, out);
}

/* Park the current coroutine on shutdown finalization; fills `out`. */
static void temporal_run_worker_finalize(temporal_php_handle_t *handle, temporal_outcome_t *out)
{
	zend_coroutine_t *co = ZEND_ASYNC_CURRENT_COROUTINE;

	temporal_call_t *call = temporal_call_new();
	if (call == NULL) {
		out->cancelled = true;
		memset(&out->r, 0, sizeof(out->r));
		return;
	}

	call->handle = handle;
	temporal_php_handle_addref(handle);

	ZEND_ASYNC_WAKER_NEW(co);
	zend_async_resume_when(co, &call->trigger->base, false,
	                       zend_async_waker_callback_resolve, NULL);
	call->trigger->base.start(&call->trigger->base);

	temporal_php_worker_finalize_shutdown(handle->box, call, temporal_worker_op_done);
	temporal_call_collect(call, co, out);
}

/* ====================================================================== */
/* TrueAsync\Temporal\Core\Connection                                     */
/* ====================================================================== */

PHP_METHOD(TrueAsync_Temporal_Core_Connection, __construct)
{
	zend_string *address = NULL, *identity = NULL, *api_key = NULL;
	zend_string *tls_root_ca = NULL, *tls_cert = NULL, *tls_key = NULL, *tls_server_name = NULL;
	bool tls = false;

	ZEND_PARSE_PARAMETERS_START(1, 8)
		Z_PARAM_STR(address)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(identity)
		Z_PARAM_STR_OR_NULL(api_key)
		Z_PARAM_BOOL(tls)
		Z_PARAM_STR_OR_NULL(tls_root_ca)
		Z_PARAM_STR_OR_NULL(tls_cert)
		Z_PARAM_STR_OR_NULL(tls_key)
		Z_PARAM_STR_OR_NULL(tls_server_name)
	ZEND_PARSE_PARAMETERS_END();

	TEMPORAL_ENSURE_COROUTINE();

	const char *addr = ZSTR_VAL(address);
	char *target_url;
	if (strstr(addr, "://") != NULL) {
		target_url = estrdup(addr);
	} else {
		zend_string *u = zend_strpprintf(0, "%s%s", tls ? "https://" : "http://", addr);
		target_url = estrdup(ZSTR_VAL(u));
		zend_string_release(u);
	}

	/* A non-empty identity is required for workers; default to "<pid>@<host>". */
	char identity_buf[300];
	const char *identity_val;
	if (identity != NULL) {
		identity_val = ZSTR_VAL(identity);
	} else {
		char hostname[256];
		if (gethostname(hostname, sizeof(hostname)) != 0) {
			strcpy(hostname, "localhost");
		}
		hostname[sizeof(hostname) - 1] = '\0';
		snprintf(identity_buf, sizeof(identity_buf), "%d@%s", (int) getpid(), hostname);
		identity_val = identity_buf;
	}

	temporal_php_connect_params_t params;
	memset(&params, 0, sizeof(params));
	params.target_url     = target_url;
	params.client_name    = "temporal-php";
	params.client_version = PHP_TEMPORAL_VERSION;
	params.identity       = identity_val;
	params.api_key        = api_key ? ZSTR_VAL(api_key) : NULL;

	if (tls) {
		params.tls_enabled = true;
		params.tls_server_root_ca_cert = tls_root_ca ? ZSTR_VAL(tls_root_ca) : NULL;
		params.tls_client_cert         = tls_cert ? ZSTR_VAL(tls_cert) : NULL;
		params.tls_client_private_key  = tls_key ? ZSTR_VAL(tls_key) : NULL;
		params.tls_server_name         = tls_server_name ? ZSTR_VAL(tls_server_name) : NULL;
	}

	temporal_outcome_t out;
	temporal_run_connect(&params, &out);

	efree(target_url);

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.connection == NULL) {
		zend_throw_exception_ex(temporal_ce_connection_exception, 0,
			"Failed to connect to Temporal: %s",
			out.r.fail != NULL ? out.r.fail : "unknown error");
		free(out.r.fail);
		RETURN_THROWS();
	}

	temporal_connection_obj *self = temporal_connection_from_obj(Z_OBJ_P(ZEND_THIS));
	self->connection = temporal_php_handle_new(out.r.connection, temporal_php_connection_free);
	if (self->connection == NULL) {
		temporal_php_connection_free(out.r.connection);
		zend_throw_error(NULL, "temporal: out of memory");
		RETURN_THROWS();
	}
}

PHP_METHOD(TrueAsync_Temporal_Core_Connection, rpcCall)
{
	zend_long service, timeout_ms = 0;
	zend_string *method, *request;
	HashTable *metadata = NULL;

	ZEND_PARSE_PARAMETERS_START(3, 5)
		Z_PARAM_LONG(service)
		Z_PARAM_STR(method)
		Z_PARAM_STR(request)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(timeout_ms)
		Z_PARAM_ARRAY_HT_OR_NULL(metadata)
	ZEND_PARSE_PARAMETERS_END();

	temporal_connection_obj *self = temporal_connection_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->connection == NULL) {
		zend_throw_error(NULL, "Temporal connection is not established");
		RETURN_THROWS();
	}

	TEMPORAL_ENSURE_COROUTINE();

	if (timeout_ms < 0 || timeout_ms > UINT32_MAX) {
		zend_value_error("rpcCall timeout must be between 0 and %u milliseconds", UINT32_MAX);
		RETURN_THROWS();
	}

	zend_string **meta_zstrs = NULL;
	const char **meta_ptrs = NULL;
	size_t meta_count = 0;
	if (metadata != NULL && zend_hash_num_elements(metadata) > 0) {
		meta_count = temporal_flatten_metadata(metadata, &meta_zstrs, &meta_ptrs);
		if (meta_count == (size_t) -1) {
			RETURN_THROWS();                     /* ValueError already thrown */
		}
	}

	temporal_outcome_t out;
	temporal_run_rpc((temporal_php_handle_t *) self->connection, (int) service, ZSTR_VAL(method),
	                 (const uint8_t *) ZSTR_VAL(request), ZSTR_LEN(request),
	                 (uint32_t) timeout_ms, meta_ptrs, meta_count, &out);

	for (size_t i = 0; i < meta_count; i++) {
		zend_string_release(meta_zstrs[i]);
	}
	if (meta_zstrs != NULL) {
		efree(meta_zstrs);
	}
	if (meta_ptrs != NULL) {
		efree(meta_ptrs);
	}

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.data == NULL) {
		const char *msg = out.r.fail != NULL ? out.r.fail : "rpc call failed";

		if (out.r.status != 0) {
			/* gRPC error: a ServiceException carrying the serialized status so
			 * the SDK can map specific failures from its details. */
			zval ex;
			object_init_ex(&ex, temporal_ce_service_exception);
			zend_update_property_string(zend_ce_exception, Z_OBJ(ex),
				"message", sizeof("message") - 1, msg);
			zend_update_property_long(zend_ce_exception, Z_OBJ(ex),
				"code", sizeof("code") - 1, (zend_long) out.r.status);
			if (out.r.details != NULL) {
				zend_update_property_stringl(temporal_ce_service_exception, Z_OBJ(ex),
					"statusDetails", sizeof("statusDetails") - 1,
					(const char *) out.r.details, out.r.details_len);
			}
			zend_throw_exception_object(&ex);
		} else {
			zend_throw_exception_ex(temporal_ce_connection_exception, 0, "%s", msg);
		}

		free(out.r.fail);
		free(out.r.details);
		RETURN_THROWS();
	}

	RETVAL_STRINGL((const char *) out.r.data, out.r.data_len);
	temporal_php_response_free(out.r.owner);
	free(out.r.fail);
	free(out.r.details);   /* normally NULL on success */
}

/* ====================================================================== */
/* TrueAsync\Temporal\Core\Worker                                         */
/* ====================================================================== */

/* Read one numeric tuning option ('key' => int in [min, max]) into *out.
 * Returns false with a ValueError thrown on a bad value. */
static bool temporal_worker_option(HashTable *options, const char *key,
                                   zend_long min, zend_long max, uint64_t *out)
{
	zval *v = zend_hash_str_find_deref(options, key, strlen(key));

	if (v == NULL) {
		return true;                             /* keep the default */
	}

	if (Z_TYPE_P(v) != IS_LONG || Z_LVAL_P(v) < min || Z_LVAL_P(v) > max) {
		zend_value_error("Worker option '%s' must be an integer between " ZEND_LONG_FMT
			" and " ZEND_LONG_FMT, key, min, max);
		return false;
	}

	*out = (uint64_t) Z_LVAL_P(v);
	return true;
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, __construct)
{
	zval *connection_zv;
	zend_string *task_queue, *namespace = NULL;
	zend_long max_activities = 100;
	HashTable *options = NULL;

	ZEND_PARSE_PARAMETERS_START(2, 5)
		Z_PARAM_OBJECT_OF_CLASS(connection_zv, temporal_ce_connection)
		Z_PARAM_STR(task_queue)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(namespace)
		Z_PARAM_LONG(max_activities)
		Z_PARAM_ARRAY_HT_OR_NULL(options)
	ZEND_PARSE_PARAMETERS_END();

	temporal_connection_obj *conn = temporal_connection_from_obj(Z_OBJ_P(connection_zv));

	if (conn->connection == NULL) {
		zend_throw_error(NULL, "Temporal connection is not established");
		RETURN_THROWS();
	}

	if (max_activities < 1 || max_activities > UINT32_MAX) {
		zend_value_error("maxConcurrentActivities must be a positive 32-bit integer");
		RETURN_THROWS();
	}

	temporal_php_worker_options_t opts = {
		.activity_slots = (uint32_t) max_activities,
		.workflow_slots = 100,
		.local_activity_slots = 100,
		.nexus_slots = 100,
		.max_cached_workflows = 1000,
		.sticky_schedule_to_start_ms = 10000,
		.graceful_shutdown_ms = 0,
		.activity_pollers = 5,
		.workflow_pollers = 2,
		.nexus_pollers = 1,
	};

	if (options != NULL) {
		uint64_t v;

#define TPHP_WORKER_OPT32(key, min, field) do { \
		v = opts.field; \
		if (!temporal_worker_option(options, key, min, (zend_long) UINT32_MAX, &v)) { \
			RETURN_THROWS(); \
		} \
		opts.field = (uint32_t) v; \
	} while (0)
#define TPHP_WORKER_OPT64(key, min, field) do { \
		v = opts.field; \
		if (!temporal_worker_option(options, key, min, ZEND_LONG_MAX, &v)) { \
			RETURN_THROWS(); \
		} \
		opts.field = v; \
	} while (0)

		TPHP_WORKER_OPT32("workflowSlots",      1, workflow_slots);
		TPHP_WORKER_OPT32("localActivitySlots", 1, local_activity_slots);
		TPHP_WORKER_OPT32("nexusSlots",         1, nexus_slots);
		TPHP_WORKER_OPT32("maxCachedWorkflows", 0, max_cached_workflows);
		TPHP_WORKER_OPT64("stickyScheduleToStartTimeoutMs", 1, sticky_schedule_to_start_ms);
		TPHP_WORKER_OPT64("gracefulShutdownMs", 0, graceful_shutdown_ms);
		TPHP_WORKER_OPT32("activityPollers",    1, activity_pollers);
		TPHP_WORKER_OPT32("workflowPollers",    1, workflow_pollers);
		TPHP_WORKER_OPT32("nexusPollers",       1, nexus_pollers);

#undef TPHP_WORKER_OPT32
#undef TPHP_WORKER_OPT64
	}

	char *err = NULL;
	void *worker = temporal_php_worker_new(((temporal_php_handle_t *) conn->connection)->box,
	                               namespace != NULL ? ZSTR_VAL(namespace) : "default",
	                               ZSTR_VAL(task_queue), &opts, &err);

	if (worker == NULL) {
		zend_throw_exception_ex(temporal_ce_temporal_exception, 0,
			"Failed to create Temporal worker: %s", err != NULL ? err : "unknown error");
		free(err);
		RETURN_THROWS();
	}

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));
	self->worker = temporal_php_handle_new(worker, temporal_php_worker_free);
	if (self->worker == NULL) {
		temporal_php_worker_free(worker);
		zend_throw_error(NULL, "temporal: out of memory");
		RETURN_THROWS();
	}
	self->connection = Z_OBJ_P(connection_zv);
	GC_ADDREF(self->connection);   /* keep the connection alive for the worker */
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, pollActivityTask)
{
	ZEND_PARSE_PARAMETERS_NONE();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->worker == NULL || self->finalized) {
		zend_throw_error(NULL, "pollActivityTask must be called on a live worker");
		RETURN_THROWS();
	}
	TEMPORAL_ENSURE_COROUTINE();

	temporal_outcome_t out;
	temporal_run_worker_poll((temporal_php_handle_t *) self->worker, temporal_php_worker_poll_activity, &out);

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.fail != NULL) {
		zend_throw_exception_ex(temporal_ce_service_exception, 0, "%s", out.r.fail);
		free(out.r.fail);
		RETURN_THROWS();
	}

	if (out.r.data == NULL) {
		RETURN_NULL();   /* shutdown */
	}

	RETVAL_STRINGL((const char *) out.r.data, out.r.data_len);
	temporal_php_response_free(out.r.owner);
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, pollWorkflowActivation)
{
	ZEND_PARSE_PARAMETERS_NONE();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->worker == NULL || self->finalized) {
		zend_throw_error(NULL, "pollWorkflowActivation must be called on a live worker");
		RETURN_THROWS();
	}
	TEMPORAL_ENSURE_COROUTINE();

	temporal_outcome_t out;
	temporal_run_worker_poll((temporal_php_handle_t *) self->worker, temporal_php_worker_poll_workflow, &out);

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.fail != NULL) {
		zend_throw_exception_ex(temporal_ce_service_exception, 0, "%s", out.r.fail);
		free(out.r.fail);
		RETURN_THROWS();
	}

	if (out.r.data == NULL) {
		RETURN_NULL();   /* shutdown */
	}

	RETVAL_STRINGL((const char *) out.r.data, out.r.data_len);
	temporal_php_response_free(out.r.owner);
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, completeActivityTask)
{
	zend_string *completion;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(completion)
	ZEND_PARSE_PARAMETERS_END();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->worker == NULL || self->finalized) {
		zend_throw_error(NULL, "completeActivityTask must be called on a live worker");
		RETURN_THROWS();
	}
	TEMPORAL_ENSURE_COROUTINE();

	temporal_outcome_t out;
	temporal_run_worker_complete((temporal_php_handle_t *) self->worker, temporal_php_worker_complete_activity,
	                             (const uint8_t *) ZSTR_VAL(completion), ZSTR_LEN(completion), &out);

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.fail != NULL) {
		zend_throw_exception_ex(temporal_ce_service_exception, 0, "%s", out.r.fail);
		free(out.r.fail);
		RETURN_THROWS();
	}
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, completeWorkflowActivation)
{
	zend_string *completion;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(completion)
	ZEND_PARSE_PARAMETERS_END();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->worker == NULL || self->finalized) {
		zend_throw_error(NULL, "completeWorkflowActivation must be called on a live worker");
		RETURN_THROWS();
	}
	TEMPORAL_ENSURE_COROUTINE();

	temporal_outcome_t out;
	temporal_run_worker_complete((temporal_php_handle_t *) self->worker, temporal_php_worker_complete_workflow,
	                             (const uint8_t *) ZSTR_VAL(completion), ZSTR_LEN(completion), &out);

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.fail != NULL) {
		zend_throw_exception_ex(temporal_ce_service_exception, 0, "%s", out.r.fail);
		free(out.r.fail);
		RETURN_THROWS();
	}
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, recordActivityHeartbeat)
{
	zend_string *heartbeat;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(heartbeat)
	ZEND_PARSE_PARAMETERS_END();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	/* No coroutine requirement: recording is synchronous in the core (in-memory
	 * store; throttling and the server RPC run on the core's own threads). */
	if (self->worker == NULL || self->finalized) {
		zend_throw_error(NULL, "recordActivityHeartbeat must be called on a live worker");
		RETURN_THROWS();
	}

	char *fail = temporal_php_worker_record_heartbeat(((temporal_php_handle_t *) self->worker)->box,
	                                          (const uint8_t *) ZSTR_VAL(heartbeat),
	                                          ZSTR_LEN(heartbeat));

	if (fail != NULL) {
		zend_throw_exception_ex(temporal_ce_service_exception, 0, "%s", fail);
		free(fail);
		RETURN_THROWS();
	}
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, initiateShutdown)
{
	ZEND_PARSE_PARAMETERS_NONE();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->worker != NULL && !self->finalized) {
		temporal_php_worker_initiate_shutdown(((temporal_php_handle_t *) self->worker)->box);
	}
}

PHP_METHOD(TrueAsync_Temporal_Core_Worker, finalizeShutdown)
{
	ZEND_PARSE_PARAMETERS_NONE();

	temporal_worker_obj *self = temporal_worker_from_obj(Z_OBJ_P(ZEND_THIS));

	if (self->worker == NULL || self->finalized) {
		zend_throw_error(NULL, "finalizeShutdown must be called on a live worker");
		RETURN_THROWS();
	}
	TEMPORAL_ENSURE_COROUTINE();

	/* No settle race here: our vendored core (true-async/sdk-rust) drops each
	 * poll/complete task's worker clone before waking us (notify-after-release),
	 * so finalize's Arc::try_unwrap deterministically sees sole ownership. */
	temporal_outcome_t out;
	temporal_run_worker_finalize((temporal_php_handle_t *) self->worker, &out);

	/* The spawned core task takes its inner worker unconditionally (before the
	 * try_unwrap branch and regardless of our cancellation), so the worker is
	 * spent the moment finalize is launched. Mark it so every later op rejects
	 * instead of unwrapping None and aborting (the handle is freed in free_obj). */
	self->finalized = true;

	if (out.cancelled) {
		RETURN_THROWS();
	}

	if (out.r.fail != NULL) {
		zend_throw_exception_ex(temporal_ce_service_exception, 0, "%s", out.r.fail);
		free(out.r.fail);
		RETURN_THROWS();
	}
}
