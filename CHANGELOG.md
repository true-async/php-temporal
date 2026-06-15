# Changelog

All notable user-facing changes to this project are documented here.
The project is pre-release; the public API is not yet stable.

## [Unreleased]

This extension is a thin native transport. The high-level client API is the
reused official Temporal PHP SDK (the `Temporal\*` namespace) running on this
transport through a `ServiceClientInterface` adapter — see the
[`true-async/sdk-php`](https://github.com/true-async/sdk-php) fork (branch
`true-async`).

### Added
- Links the official Temporal Rust core (`sdk-core-c-bridge`), vendored as a
  submodule under `third_party/sdk-rust`. Points at our fork
  (`true-async/sdk-rust`, branch `true-async`): v0.4.0 plus one fix to the
  c-bridge worker shutdown (see Fixed), submitted upstream. The bridge header is
  isolated in a Zend-free shim (`temporal_core.c`) to avoid symbol clashes with
  `php.h`.
- A process-wide core runtime (Tokio), created in MINIT.
- `TrueAsync\Temporal\Core\Connection` — the transport: `connect` plus an async
  `rpcCall(service, method, requestBytes): responseBytes`. Each call parks the
  current coroutine while the core runs the gRPC on its own threads and resumes
  it through a cross-thread trigger; the Tokio-side callback never touches
  Zend/TSRM. Verified end to end against a live dev server (the reused
  `WorkflowClient` starts a workflow over this transport).
- Transport exceptions: `TemporalException`, `ConnectionException`,
  `ServiceException`.
- `TrueAsync\Temporal\Core\Worker` — the worker transport, handling both workflow
  and activity tasks: `create` plus async `pollActivityTask(): ?string` /
  `pollWorkflowActivation(): ?string` (the Rust core long-polls the server on its
  threads; the call parks the coroutine until a task is ready, or returns null on
  shutdown), `completeActivityTask(bytes)` / `completeWorkflowActivation(bytes)`,
  `initiateShutdown()` and `finalizeShutdown()`. The task/activation bytes are
  coresdk protobuf, decoded in PHP. After `initiateShutdown()`, drain every poll
  in use (each returns null) before `finalizeShutdown()`, which waits for all
  enabled pollers to wind down.
- The connection now defaults its identity to `<pid>@<hostname>` when none is
  given (required by workers).
- `Core\Worker::recordActivityHeartbeat(bytes)` — records an activity heartbeat
  (serialized coresdk ActivityHeartbeat). Synchronous and never parks the
  coroutine: the core stores the heartbeat in memory and throttles/sends it to
  the server on its own threads. A pending cancellation is delivered separately,
  as a cancel-variant task from `pollActivityTask()`.
- The worker now caches workflow runs (sticky execution) so a fired timer or
  resolved activity resumes the live instance instead of replaying from scratch.
- The worker now dispatches local activities: a workflow can run them, and the
  core executes them in-process and delivers them on `pollActivityTask()` like
  regular activities.

- `Core\Worker` now takes a tuning `$options` array (slot-supplier sizes, sticky
  cache size, sticky schedule-to-start timeout, graceful shutdown period, poller
  maximums) instead of hardcoding those values; out-of-range or mistyped values
  are rejected with a `ValueError`.

### Fixed
- `finalizeShutdown()` no longer intermittently fails with "Cannot finalize,
  expected 1 reference, got 2" under load. The upstream c-bridge dropped each
  poll/complete task's worker `Arc` clone only after waking the lang thread, so a
  finalize racing that wake saw a transient extra owner and `Arc::try_unwrap`
  failed (spending the worker beyond retry). Our core fork drops the clone before
  the callback (notify-after-release), so finalize deterministically sees sole
  ownership. Submitted upstream.
- `rpcCall` metadata keys/values embedding a newline are rejected with a
  `ValueError` instead of corrupting the core's `key\nvalue` wire form; a
  negative timeout is rejected instead of being silently cast to a huge uint32.
- Allocation failures in the cross-thread call bridge no longer NULL-deref
  (Zend-side sites throw; the Zend-free shim delivers a synchronous
  out-of-memory failure through the operation's own callback).
- Calling any `Core\Worker` method after `finalizeShutdown()` (including a second
  `finalizeShutdown()`) no longer aborts the process; it now throws.
- A core handle (`Core\Connection` / `Core\Worker`) is now refcounted and kept
  alive until its in-flight call completes. Cancelling a coroutine parked in
  `rpcCall()` / `poll*()` can no longer free the handle while the core thread is
  still using it (use-after-free).
- Cancelling a coroutine parked in `rpcCall()` now aborts the in-flight gRPC call
  promptly through a core cancellation token, instead of leaving it to run to
  completion (e.g. a long-poll holding the handle for up to a minute).
