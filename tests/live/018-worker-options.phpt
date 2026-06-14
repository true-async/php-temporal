--TEST--
Core\Worker constructor accepts tuning options and validates them
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

await(spawn(function () {
    $conn = new Connection(temporal_test_address());

    // Custom tuning goes through the full lifecycle.
    $worker = new Worker($conn, 'opts-' . bin2hex(random_bytes(3)), temporal_test_namespace(), 8, [
        'workflowSlots' => 16,
        'localActivitySlots' => 4,
        'maxCachedWorkflows' => 50,
        'stickyScheduleToStartTimeoutMs' => 5000,
        'gracefulShutdownMs' => 1000,
        'activityPollers' => 2,
        'workflowPollers' => 2,
    ]);
    $worker->initiateShutdown();
    $worker->pollWorkflowActivation();
    $worker->pollActivityTask();
    $worker->finalizeShutdown();
    var_dump('lifecycle ok');

    // Out-of-range and mistyped values are rejected before the core sees them.
    try {
        new Worker($conn, 'opts-bad', temporal_test_namespace(), 8, ['workflowSlots' => 0]);
    } catch (ValueError $e) {
        var_dump($e->getMessage());
    }
    try {
        new Worker($conn, 'opts-bad', temporal_test_namespace(), 8, ['activityPollers' => 'many']);
    } catch (ValueError $e) {
        var_dump($e->getMessage());
    }
    try {
        new Worker($conn, 'opts-bad', temporal_test_namespace(), 0);
    } catch (ValueError $e) {
        var_dump($e->getMessage());
    }
}));
?>
--EXPECT--
string(12) "lifecycle ok"
string(73) "Worker option 'workflowSlots' must be an integer between 1 and 4294967295"
string(75) "Worker option 'activityPollers' must be an integer between 1 and 4294967295"
string(57) "maxConcurrentActivities must be a positive 32-bit integer"
