--TEST--
Core\Worker pollWorkflowActivation / shutdown lifecycle against a live server
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
use function Async\spawn;
use function Async\await;

$out = await(spawn(function () {
    $conn = new Connection(temporal_test_address());
    $worker = new Worker($conn, 'phase3-lifecycle-' . bin2hex(random_bytes(3)), temporal_test_namespace(), 4);

    // Nothing schedules work here. After initiating shutdown each poll yields
    // null; finalize waits for every enabled poller (workflow and activity) to
    // have drained, so both must be polled first.
    $worker->initiateShutdown();
    $activation = $worker->pollWorkflowActivation();
    $task = $worker->pollActivityTask();
    $worker->finalizeShutdown();

    return [$activation, $task];
}));

var_dump($out[0] === null, $out[1] === null);
?>
--EXPECT--
bool(true)
bool(true)
