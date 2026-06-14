--TEST--
Core\Worker: operations after finalizeShutdown throw instead of aborting (unwrap-on-None)
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
    $worker = new Worker($conn, 'f2-' . bin2hex(random_bytes(3)), temporal_test_namespace(), 4);

    $worker->initiateShutdown();
    $worker->pollActivityTask();   // drains to null (shutdown)
    $worker->finalizeShutdown();   // the core takes its inner worker -> None

    // Once finalized the core handle is spent. Any further call used to reach a
    // bridge unwrap() on None and abort the process; it must now throw cleanly.
    try {
        $worker->pollActivityTask();
        echo "BUG: poll after finalize did not throw\n";
    } catch (\Error $e) {
        echo "poll: threw\n";
    }

    try {
        $worker->finalizeShutdown();
        echo "BUG: 2nd finalize did not throw\n";
    } catch (\Error $e) {
        echo "finalize: threw\n";
    }

    $worker->initiateShutdown();   // graceful no-op, no abort
    echo "initiate: ok\n";
}));

echo "survived\n";
?>
--EXPECT--
poll: threw
finalize: threw
initiate: ok
survived
