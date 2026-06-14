# php-temporal — design

Native asynchronous Temporal client and worker for **PHP TrueAsync**, built on
the **official Temporal Rust Core**. This document is the working design; it is
meant to be revised collaboratively, not treated as final.

## 1. Why this is buildable, and how official it is

Temporal ships a shared **Rust Core** (`temporalio/sdk-rust`, formerly
`sdk-core`) that already powers the official TypeScript, Python, .NET and Ruby
SDKs in production. The core exposes a **C ABI** through the
`sdk-core-c-bridge` crate (`crates/sdk-core-c-bridge/include/temporal-sdk-core-c-bridge.h`),
which compiles to `libtemporalio_sdk_core_c_bridge.{so,dylib,dll}`. The .NET and
Ruby SDKs link this exact surface.

That C bridge is our integration target. We do **not** write Rust ourselves and
we do **not** depend on RoadRunner (the transport the official
`temporalio/sdk-php` SDK uses). We link the official C ABI and drive it from a
PHP extension over the TrueAsync reactor.

Officialness, component by component:

| Component | Status | Our use |
| --- | --- | --- |
| `temporalio/sdk-rust` core + `sdk-core-c-bridge` | Official, production (backs 4 shipping SDKs) | **Base we link** |
| `temporalio-sdk` (high-level Rust SDK) | Official, Public Preview | Reference only, not linked |
| `temporalio/sdk-php` (RoadRunner-based) | Official | API/semantics reference, not a dependency |

## 2. Layering

```
┌──────────────────────────────────────────────────────────────┐
│ PHP SDK            TrueAsync\Temporal\{Client, WorkflowHandle, │
│ (composer pkg)     Worker, options, enums, exceptions,        │
│                    DataConverter}  +  generated Temporal\Api\* │
├──────────────────────────────────────────────────────────────┤
│ PHP ext (C)        thin core: runtime + Core\Connection        │
│                    (connect, rpcCall) — bytes in → bytes out,  │
│                    cross-thread trigger → coroutine resume     │
├──────────────────────────────────────────────────────────────┤
│ C ABI              temporal-sdk-core-c-bridge.h                │
├──────────────────────────────────────────────────────────────┤
│ Rust core          client, worker, state machines, Tokio rt    │
│ (its own threads)  gRPC to the Temporal frontend               │
└──────────────────────────────────────────────────────────────┘
```

**Split (resolved, see §9.6):** the C extension is a *thin core* — runtime plus
`Core\Connection` exposing `connect` and an async `rpcCall(service, method,
bytes): bytes`. Everything user-facing — the high-level `Client`/`WorkflowHandle`,
the typed options/enums/exceptions, the generated `Temporal\Api\*` protobuf
messages and the pluggable data converter — lives in a **PHP composer package**.

This is how every other sdk-core-based SDK (and the official sdk-php) is built: a
minimal native bridge plus a language-side SDK. It keeps the hand-written C tiny
(just the cross-thread bridge — the hard, race-prone part), puts protobuf where
it is naturally consumed and extensible (interceptors, read-models, codec
chains), and lets the option objects use real constructor promotion. Wire
messages for the client path are transient, so building them in PHP costs only a
few short-lived objects — negligible against the network round-trip.

## 3. The C bridge in one paragraph

The whole bridge is **callback based with a `void *user_data`** and a single
shared **Tokio runtime**. Representative signatures:

```c
TemporalCoreRuntimeOrFail temporal_core_runtime_new(const TemporalCoreRuntimeOptions*);
void temporal_core_runtime_free(TemporalCoreRuntime*);
void temporal_core_byte_array_free(TemporalCoreRuntime*, const TemporalCoreByteArray*);

void temporal_core_client_connect(TemporalCoreRuntime*, const TemporalCoreClientConnectOptions*,
                                  void *user_data, TemporalCoreClientConnectCallback);
void temporal_core_client_rpc_call(TemporalCoreConnection*, const TemporalCoreRpcCallOptions*,
                                   void *user_data, TemporalCoreClientRpcCallCallback);
void temporal_core_client_free(TemporalCoreConnection*);

// worker poll / complete are likewise async + callback:
void temporal_core_worker_poll_workflow_activation(TemporalCoreWorker*, void*, TemporalCoreWorkerPollCallback);
void temporal_core_worker_complete_workflow_activation(TemporalCoreWorker*, TemporalCoreByteArrayRef, void*, TemporalCoreWorkerCallback);
// + activity / nexus variants, initiate_shutdown, finalize_shutdown, cancellation_token_*
```

Each async function returns immediately. The callback fires later **on a Tokio
worker thread** (not the PHP/reactor thread), carrying either a `success` byte
array or a `failure_message` / `failure_details` pair. Returned byte arrays are
owned by Rust and must be freed with `temporal_core_byte_array_free`.

## 4. Threading model — the heart of the integration

TrueAsync runs PHP on a single reactor thread. The Rust core runs gRPC on its
own Tokio threads. The bridge between them is the existing TrueAsync primitive
**`zend_async_trigger_event_t`** (what we informally call the "trigger"):

* it wraps a libuv `uv_async_t`;
* its `trigger(event)` call is just `uv_async_send()`, the one libuv API that is
  **safe to call from any thread**;
* when the reactor next spins, `on_trigger_event` runs on the reactor thread and
  fires `ZEND_ASYNC_CALLBACKS_NOTIFY`, which resumes whatever coroutine was
  waiting.

This is the same shape `ext/async` already uses for its **cross-thread future**
(`future.c`, "Shared future state — cross-thread future bridge") and thread
pool. We reuse that proven path rather than invent one.

### Rule: do the byte work outside the coroutine

> If it can be done in a C callback outside a coroutine, do it there.

A single RPC therefore flows like this, and **only the final hop touches a
coroutine**:

```
 reactor thread                     Tokio thread
 ──────────────                     ────────────
 $handle->getResult()
   build request protobuf (PHP)
   alloc result slot + trigger
   suspend coroutine  ───────►  temporal_core_client_rpc_call(..., user_data=slot, cb)
   (coroutine parked)               ... gRPC in flight ...
                                  cb(user_data, success/fail):           ← on Tokio thread
                                    store byte ptr into slot
                                    trigger->trigger()  = uv_async_send  ← thread-safe wake
   on_trigger_event ◄───────────────────────────────────────────────────
     NOTIFY → resume coroutine
     decode protobuf (PHP)
     temporal_core_byte_array_free(rt, bytes)
     return zval / throw
```

The Tokio-side callback does no PHP work, takes no Zend locks, allocates nothing
Zend-owned. It only stashes a pointer and pulls the trigger. All Zend-touching
work happens back on the reactor thread inside the resumed coroutine. This keeps
us clear of TSRM/GC races and matches the project rule above.

Two building blocks, pick per call site:

* **One-shot RPC** (start, signal, query, getResult): a **thread-safe future**
  (`zend_async_new_future_t(thread_safe = true, ...)`). The Tokio callback
  completes or rejects it; the coroutine `await`s it. Cleanest for request →
  response.
* **Repeated poll loop** (worker): a raw trigger plus a small MPSC-style queue,
  so successive poll completions can be drained without re-arming a fresh future
  each time.

### Cancellation

`temporal_core_cancellation_token_new/cancel/free` maps onto TrueAsync
cancellation. When the awaiting coroutine is cancelled, we call
`temporal_core_cancellation_token_cancel` so the in-flight RPC unwinds in the
core; the callback still fires (with a cancelled failure) and still pulls the
trigger, so there is no lost wake. The token is freed on the reactor thread
after the callback has run, mirroring the flock-cancel ownership pattern in
`ext/async` (the in-flight op keeps the slot alive until its callback returns).

## 5. Memory & lifecycle ownership

* **Runtime**: one `TemporalCoreRuntime` per process (module globals), created in
  MINIT after the reactor exists, freed in MSHUTDOWN after `quiesce`.
* **Client/Connection**: created per `Client` object; `temporal_core_client_free`
  in the object free handler. The connect is itself async (callback) so the
  constructor suspends the coroutine until connected.
* **Byte arrays**: every `success`/`failure_details` buffer returned by the core
  is freed with `temporal_core_byte_array_free(runtime, buf)` once copied into a
  PHP string, on the reactor thread.
* **Result slot**: the per-call struct lives in the trigger's `extra_size` tail
  (`ZEND_ASYNC_NEW_TRIGGER_EVENT_EX(sizeof(slot))`) or in the thread-safe future
  state, so its lifetime is tied to the awaited event, not the C stack.

## 6. Protobuf strategy

The Temporal API surface (`temporal.api.workflowservice.v1.*`, `common.v1`,
`enums.v1`, `failure.v1`, ...) is large. We do not hand-roll it.

Plan: vendor the proto set from the core repo (`crates/sdk-core-protos` /
upstream `temporalio/api`), generate PHP classes with `protoc --php_out` against
the `google/protobuf` PHP runtime, and ship them under
`TrueAsync\Temporal\Api\...`. The extension only moves serialized bytes; PHP owns
the message types. The data converter (payload encode/decode, default JSON +
binary) is a PHP concern as well, pluggable via the `dataConverter` config key.

Decided: the generated proto classes are **bundled** in the repo (regenerated by
a script when the vendored core is bumped), matching how PECL extensions ship
generated arginfo. The `protobuf` C extension is the encoder, with a pure-PHP
fallback acceptable.

## 7. Phasing

**Phase 1 — Client (starter).** `connect` + `rpc_call` only. Start / signal /
query / getResult / describe / cancel / terminate. High value, low complexity,
no determinism concerns. This is the concrete surface in `temporal.stub.php`.

**Phase 2 — Activity worker.** Poll activity tasks, run the PHP activity in a
normal TrueAsync coroutine (real I/O, this is exactly what TrueAsync is good
at), complete back through the core. Heartbeating maps to a timer + RPC.

**Phase 3 — Workflow worker (the hard part).** Temporal workflows must be
**deterministic and replayable**: time, randomness and every await must come
from history, never from the live reactor.

**Chosen model: blocking coroutine, Go/Java style.** Workflow code is ordinary
"write sync" PHP (no `yield`); it runs on a deterministic dispatcher we own,
built on TrueAsync's existing **coroutines, channels and suspend** primitives.
The dispatcher runs exactly one workflow coroutine at a time and only ever
resumes it from history, so execution is single-threaded and deterministic. This
is how the Go and Java SDKs work, and it matches TrueAsync's own "write sync, run
async" identity. The generator/`yield` model of `temporalio/sdk-php` is the
fallback only if detection proves unreliable.

Determinism rests on three layers, taken from Go:

1. **A user-space deterministic dispatcher** over TrueAsync coroutines and
   channels; the only way a workflow waits is a `Workflow::*` call that suspends
   into it (cf. Go's `coroutineState` gated by channels, single coroutine
   running at a time).
2. **Blessed primitives** — `Workflow::executeActivity` / `timer` / `awaitSignal`
   / `now` / `sideEffect` / `go` — replace anything touching time, I/O or random.
3. **Detection, not a hard sandbox** — a runtime guard trips if a workflow
   coroutine reaches the real reactor (real `Async\*`, sockets, timers), backed
   by replay-mismatch detection; an optional static analyzer can follow later
   (cf. Go's `workflowcheck`).

The core already helps: it hands us "workflow activations" (jobs) and takes back
"activation completions" (commands), and exposes a deterministic RNG via the
bridge. Our job is the language-side dispatcher and the blessed primitives, not
the state machines.

**Implementation note (as shipped).** Phase 3 landed on the *fallback* path, not
the bespoke dispatcher above: rather than write a new deterministic dispatcher,
the worker reuses `temporalio/sdk-php`'s existing generator-based workflow engine
(the Router/RunningWorkflows command-queue loop) unchanged, driven one activation
at a time by `WorkflowWorkerFactory::processActivation`. Determinism is enforced
by layer 3 — a runtime guard (`NonDeterministicWorkflowException`): a sentinel
coroutine that, because the reactor is single-threaded, can only run if the
worker coroutine actually suspended into the real reactor, which legit workflow
code never does. Reusing the proven engine instead of reimplementing the state
machines is what made the lifecycle (timers, activities, signals, queries,
cancellation, child workflows, continue-as-new) land quickly and correctly;
the blessed-primitive dispatcher remains a possible future direction, not the
current design.

## 8. Build & dependencies

* `sdk-core-c-bridge` built via `cargo` as a `staticlib`/`cdylib`; vendored as a
  git submodule under `third_party/sdk-rust` (mirrors how php-clickhouse vendors
  `clickhouse-cpp` under `third_party/`).
* `config.m4` / `configure.ac`: locate Rust/cargo, build the bridge, then
  compile and link the PHP extension against the produced library + the C header
  include dir. ZTS required (TrueAsync).
* `google/protobuf` PHP runtime for the generated message classes.

## 9. Open questions for review

1. ~~Inputs as config arrays vs typed option objects.~~ **Resolved: typed
   option objects** (official-SDK style), including connection config
   (`ConnectionOptions`, `TlsOptions`, `RetryPolicy`, `StartWorkflowOptions`).
   Results stay as readonly value objects.
2. ~~Bundle vs generate the protobuf classes.~~ **Resolved: protobuf lives on
   the PHP side** (slim C bridge, bytes only); generated classes are **bundled**
   in the repo under `TrueAsync\Temporal\Api\...`; runtime uses the `protobuf` C
   extension (pure-PHP fallback acceptable). Users never see protobuf directly.
3. ~~Worker workflow model (a) vs (b).~~ **Resolved at design time: (b) blocking
   coroutine, Go/Java style** — a deterministic dispatcher over TrueAsync
   coroutines + channels + suspend, blessed `Workflow::*` primitives, and a
   detect-guard instead of a hard sandbox (section 7). **As shipped, Phase 3 took
   the generator/`yield` fallback instead** — it reuses `temporalio/sdk-php`'s
   engine unchanged, keeping only the detect-guard from (b). See the
   "Implementation note (as shipped)" in section 7.
4. ~~One process-wide runtime vs configurable.~~ **Resolved: one
   `TemporalCoreRuntime` per process** (Tokio threads), created in MINIT, freed
   in MSHUTDOWN after quiesce; all clients/workers share it. Telemetry / metrics
   exporters exposed later via an optional `RuntimeOptions`.
5. Repo name / namespace confirmed as `php-temporal` / `TrueAsync\Temporal`.
6. **Where the wire protobuf and high-level API live.** **Resolved: thin C core +
   PHP SDK (option B).** The C extension exposes only `Core\Connection`
   (`connect` + async `rpcCall`); the public `Client`/`WorkflowHandle`, options,
   enums, exceptions, the generated `Temporal\Api\*` messages and the data
   converter live in a PHP composer package. Rationale: performance is
   network-bound either way, so the only real trade is C surface vs a composer
   artifact — and a smaller, memory-safe C core wins (fewer UAF/race risks),
   amortizes toward the phase-3 worker (where PHP genuinely consumes activation
   data), and keeps the SDK extensible. The step-2 C `Client` collapses into
   `Core\Connection`; its option/enum/exception classes move to PHP.
