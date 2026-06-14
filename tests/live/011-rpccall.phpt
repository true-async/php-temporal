--TEST--
Core\Connection::rpcCall round-trips a real RPC (GetSystemInfo) via the async bridge
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

$len = await(spawn(function () {
    $conn = new Connection(temporal_test_address());
    // GetSystemInfo takes an empty request; service 1 = Workflow.
    $resp = $conn->rpcCall(1, 'GetSystemInfo', '');
    return strlen($resp);
}));

var_dump($len > 0);
?>
--EXPECT--
bool(true)
