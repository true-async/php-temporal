--TEST--
Core\Worker create / poll / shutdown lifecycle against a live server
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
    $worker = new Worker($conn, 'phase2-lifecycle-' . bin2hex(random_bytes(3)), temporal_test_namespace(), 4);

    // Nothing schedules activities here; shut down, then a poll must yield null.
    $worker->initiateShutdown();
    $task = $worker->pollActivityTask();
    $worker->finalizeShutdown();

    return $task;
}));

var_dump($out === null);
?>
--EXPECT--
bool(true)
