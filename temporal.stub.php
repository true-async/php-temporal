<?php

/**
 * @generate-class-entries
 *
 * Native Temporal transport for PHP TrueAsync, built on the official Temporal
 * Rust core (sdk-core-c-bridge).
 *
 * This extension is intentionally a thin transport: it exposes only the
 * connection and an async `rpcCall`. The high-level client API is the reused
 * official Temporal PHP SDK (the `Temporal\*` namespace, shipped as a composer
 * package) driven through a `ServiceClientInterface` adapter over
 * `TrueAsync\Temporal\Core\Connection`.
 */

namespace TrueAsync\Temporal;

/** Base exception for the native Temporal transport. */
class TemporalException extends \RuntimeException {}

/** Transport / connection failure (connect, broken stream, cancel). */
class ConnectionException extends TemporalException {}

/** Non-OK gRPC status returned by the Temporal frontend service. */
class ServiceException extends TemporalException
{
    /**
     * The serialized google.rpc.Status (the gRPC `grpc-status-details-bin`
     * trailer) carrying typed error details, or null. Used by the SDK to map a
     * status into a specific exception.
     */
    public ?string $statusDetails = null;
}

namespace TrueAsync\Temporal\Core;

/**
 * Low-level transport over the Temporal Rust core. Each call parks the current
 * coroutine while the core runs the gRPC on its own threads, then resumes it
 * through a cross-thread trigger. Called outside a coroutine (the top-level
 * flow), it launches the scheduler and runs as the main coroutine — no explicit
 * Async\spawn() wrapper is required, just like Async\await().
 */
final class Connection
{
    public function __construct(
        string $address,
        ?string $identity = null,
        ?string $apiKey = null,
        bool $tls = false,
        ?string $tlsServerRootCaCert = null,
        ?string $tlsClientCert = null,
        ?string $tlsClientPrivateKey = null,
        ?string $tlsServerName = null,
    ) {}

    /**
     * Issue a unary RPC. `$service` selects the gRPC service (1 = Workflow).
     * `$metadata` is a map of header name => value (string) or list of values,
     * forwarded as gRPC call metadata (e.g. an API key / Authorization header).
     * Returns the response protobuf bytes; throws ConnectionException /
     * ServiceException on failure.
     */
    public function rpcCall(int $service, string $method, string $request, int $timeoutMs = 0, array $metadata = []): string {}
}

/**
 * Low-level activity worker over the Temporal Rust core. The Rust core polls the
 * server (poll/respond loop) on its own threads; this class hands the polled
 * tasks (coresdk protobuf bytes) to PHP and takes back completions. The high-
 * level dispatch/execution is the reused SDK; this is only the transport.
 */
final class Worker
{
    /**
     * `$options` tunes the core worker (defaults in parentheses):
     * - workflowSlots (100), localActivitySlots (100), nexusSlots (100) —
     *   fixed-size task slot suppliers, like maxConcurrentActivities;
     * - maxCachedWorkflows (1000) — sticky cache size (a perf knob: replay
     *   stays correct at any size);
     * - stickyScheduleToStartTimeoutMs (10000);
     * - gracefulShutdownMs (0) — how long in-flight activities get after
     *   initiateShutdown() before the core cancels them;
     * - activityPollers (5), workflowPollers (2), nexusPollers (1) — poller
     *   maximums.
     */
    public function __construct(Connection $connection, string $taskQueue, string $namespace = 'default', int $maxConcurrentActivities = 100, array $options = []) {}

    /**
     * Poll for the next activity task. Parks the coroutine until a task is
     * ready; returns the serialized coresdk ActivityTask, or null once the
     * worker has shut down.
     */
    public function pollActivityTask(): ?string {}

    /** Report an activity task completion (serialized coresdk completion). */
    public function completeActivityTask(string $completion): void {}

    /**
     * Record an activity heartbeat (serialized coresdk ActivityHeartbeat).
     * Synchronous — never parks the coroutine: the core stores the heartbeat in
     * memory and throttles/sends it to the server on its own threads. A pending
     * cancellation is delivered separately, as a cancel-variant task from
     * pollActivityTask().
     */
    public function recordActivityHeartbeat(string $heartbeat): void {}

    /**
     * Poll for the next workflow activation. Parks the coroutine until an
     * activation is ready; returns the serialized coresdk WorkflowActivation, or
     * null once the worker has shut down.
     */
    public function pollWorkflowActivation(): ?string {}

    /** Report a workflow activation completion (serialized coresdk completion). */
    public function completeWorkflowActivation(string $completion): void {}

    /** Begin graceful shutdown; pending and subsequent polls return null. */
    public function initiateShutdown(): void {}

    /** Wait for shutdown to fully drain. */
    public function finalizeShutdown(): void {}
}
