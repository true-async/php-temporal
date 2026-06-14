--TEST--
Core\Connection: cancelling a coroutine parked in rpcCall is safe (handle refcount + cancel token)
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
use function Async\spawn;
use function Async\await;
use function Async\delay;

// PollWorkflowTaskQueue { namespace="default", taskQueue{name="ttq"} };
// the server long-polls, so the RPC is reliably in flight when we cancel.
$req = "\x0A\x07default\x12\x05\x0A\x03ttq";

await(spawn(function () use ($req) {
    // The connection's only reference lives inside the cancelled coroutine, so
    // its destructor runs while the RPC is still in flight. The handle refcount
    // must keep the core handle alive until the (token-aborted) call completes.
    $co = spawn(function () use ($req) {
        $conn = new Connection(temporal_test_address());
        $conn->rpcCall(1, 'PollWorkflowTaskQueue', $req, 0);
    });
    delay(200);
    $co->cancel();
    try { await($co); } catch (\Throwable $e) { /* cancelled */ }
    echo "survived cancel\n";

    // A connection outlives a cancelled in-flight RPC and stays usable.
    $conn = new Connection(temporal_test_address());
    $co2 = spawn(function () use ($conn, $req) {
        $conn->rpcCall(1, 'PollWorkflowTaskQueue', $req, 0);
    });
    delay(200);
    $co2->cancel();
    try { await($co2); } catch (\Throwable $e) { /* cancelled */ }
    $info = $conn->rpcCall(1, 'GetSystemInfo', '');
    echo (strlen($info) > 0 ? "connection reusable\n" : "BUG: empty response\n");
}));

echo "done\n";
?>
--EXPECT--
survived cancel
connection reusable
done
