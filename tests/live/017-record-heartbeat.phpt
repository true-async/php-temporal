--TEST--
Core\Worker recordActivityHeartbeat: accepts a heartbeat, rejects garbage and a finalized worker
--EXTENSIONS--
temporal
--SKIPIF--
<?php
require __DIR__ . '/../inc/temporal.inc';
temporal_skip_if_no_server();
?>
--FILE--
<?php
require __DIR__ . '/../inc/temporal.inc';

use TrueAsync\Temporal\Core\Connection;
use TrueAsync\Temporal\Core\Worker;
use TrueAsync\Temporal\ServiceException;
use function Async\spawn;
use function Async\await;

await(spawn(function () {
    $conn = new Connection(temporal_test_address());
    $worker = new Worker($conn, 'hb-' . bin2hex(random_bytes(3)), temporal_test_namespace(), 4);

    // A hand-encoded coresdk ActivityHeartbeat: field 1 (task_token) = "abc".
    // The token matches no running activity; the core records and ignores it.
    $worker->recordActivityHeartbeat("\x0a\x03abc");
    var_dump('recorded');

    // Bytes that are not a valid protobuf message: the core reports a decode
    // failure, surfaced as a ServiceException.
    try {
        $worker->recordActivityHeartbeat("\xff");
    } catch (ServiceException $e) {
        var_dump(str_contains($e->getMessage(), 'decode'));
    }

    $worker->initiateShutdown();
    $worker->pollWorkflowActivation();
    $worker->pollActivityTask();
    $worker->finalizeShutdown();

    // The finalized guard covers heartbeats too (the core would abort otherwise).
    try {
        $worker->recordActivityHeartbeat("\x0a\x03abc");
    } catch (Error $e) {
        var_dump($e->getMessage());
    }
}));
?>
--EXPECT--
string(8) "recorded"
bool(true)
string(55) "recordActivityHeartbeat must be called on a live worker"
