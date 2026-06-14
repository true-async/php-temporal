<div align="center">

# php-temporal

**Native asynchronous [Temporal](https://temporal.io/) transport for [PHP TrueAsync](https://github.com/true-async).**
*Write sync, run async.*

[![PHP](https://img.shields.io/badge/PHP-8.x_ZTS-777BB4?style=flat-square&logo=php&logoColor=white)](https://www.php.net/)
[![Temporal](https://img.shields.io/badge/Temporal-Rust_Core-000000?style=flat-square&logo=temporal&logoColor=white)](https://github.com/temporalio/sdk-rust)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue?style=flat-square)](LICENSE)

</div>

A thin native extension that runs the **official Temporal Rust Core**
([`temporalio/sdk-rust`](https://github.com/temporalio/sdk-rust)) in-process and
exposes it to PHP as an async transport. No gRPC extension, no RoadRunner: the
core's gRPC runs on its own Tokio threads, and completions are delivered back to
the TrueAsync reactor through a cross-thread trigger, so every call looks
synchronous while the coroutine yields underneath.

The **high-level client API is the reused official Temporal PHP SDK** (the
`Temporal\*` namespace), driven through a `ServiceClientInterface` adapter over
this transport — see the [`true-async/sdk-php`](https://github.com/true-async/sdk-php)
fork (branch `true-async`), which strips gRPC/RoadRunner from the dependencies.

```php
use Temporal\Client\WorkflowClient;
use Temporal\Client\WorkflowOptions;
use Temporal\Client\GRPC\TrueAsyncServiceClient;
use TrueAsync\Temporal\Core\Connection;
use function Async\spawn;
use function Async\await;

$run = await(spawn(function () {
    // Native transport over the Rust core; parks the coroutine on every RPC.
    $core   = new Connection('127.0.0.1:7233');
    $client = WorkflowClient::create(TrueAsyncServiceClient::fromCore($core));

    $stub = $client->newUntypedWorkflowStub('OrderWorkflow',
        (new WorkflowOptions())->withTaskQueue('orders'));

    return $client->start($stub, $order);
}));
```

## What this extension provides

Just the transport seam:

- `TrueAsync\Temporal\Core\Connection` — `connect` plus an async
  `rpcCall(service, method, requestBytes): responseBytes`.
- `TrueAsync\Temporal\Core\Worker` — the worker transport: poll/complete for
  activity tasks and workflow activations, activity heartbeat recording, and
  the shutdown lifecycle.
- `TrueAsync\Temporal\{TemporalException, ConnectionException, ServiceException}`.

Everything user-facing (workflow client, options, data converter, the generated
`Temporal\Api\*` protobuf messages) comes from the reused SDK.

## Status

`0.1.0-dev` — pre-release, the API is not yet stable. Working end to end against
a live server today:

- **Transport** (`Core\Connection`, `Core\Worker`) — reviewed, ASAN-clean,
  covered by the test suite and CI.
- **Activity worker** — run, heartbeat, cooperative cancellation.
- **Workflow worker** — the core lifecycle through the reused SDK engine:
  start/complete, timers, activities, signals, queries, cancellation (of the
  workflow, its timers, activities and child workflows), child workflows, and
  continue-as-new.

In progress: the long tail of workflow commands — signal/cancel external
workflow, updates, side effects, versioning/patches, local activities, and
search attributes. These raise an explicit error rather than failing silently.

## Why

The official [`temporalio/sdk-php`](https://github.com/temporalio/sdk-php) runs
PHP workers under **RoadRunner** with its own event loop, beside TrueAsync rather
than on it. This project links the **official Rust core** directly and reuses the
SDK's client layer on top, in-process on the TrueAsync reactor.

## Requirements

- PHP 8.x built with **ZTS** and the **TrueAsync** runtime.
- A Rust toolchain (`cargo`) to build the vendored `sdk-core-c-bridge`.
- The reused SDK package (`true-async/sdk-php`, branch `true-async`), which pulls
  `google/protobuf` and the bundled Temporal protobuf messages.

## Build

```sh
git submodule update --init --recursive
cargo build --release -p temporalio-sdk-core-c-bridge \
  --manifest-path third_party/sdk-rust/Cargo.toml      # build the Rust core bridge (once)
phpize && ./configure --enable-temporal --with-php-config="$(command -v php-config)"
make -j"$(nproc)"                                        # -> modules/temporal.so

php run-tests.php -q -p "$(command -v php)" \
  -d extension="$(pwd)/modules/temporal.so" tests/       # live/ cases SKIP without a dev server
```

Full instructions, requirements and the design rationale:
[docs/installation.md](docs/installation.md) and [docs/DESIGN.md](docs/DESIGN.md).

## License

Apache 2.0. Temporal and the Temporal logo are trademarks of Temporal
Technologies Inc.
