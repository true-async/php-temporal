--TEST--
Core\Connection::rpcCall forwards gRPC call metadata to the wire
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
use TrueAsync\Temporal\ConnectionException;
use function Async\spawn;
use function Async\await;

function rpc(array $meta) {
    return await(spawn(function () use ($meta) {
        $c = new Connection(temporal_test_address());
        return $c->rpcCall(1, 'GetSystemInfo', '', 0, $meta);
    }));
}

// Valid metadata (single value and a list of values) reaches the server.
var_dump(strlen(rpc(['x-custom' => 'value'])) > 0);
var_dump(strlen(rpc(['x-multi' => ['a', 'b']])) > 0);

// An invalid metadata key is rejected by the gRPC layer, proving the metadata
// is actually forwarded to the wire (not silently dropped).
try {
    rpc(['Bad Key!' => 'v']);
    echo "not rejected\n";
} catch (ConnectionException $e) {
    echo "rejected\n";
}
?>
--EXPECT--
bool(true)
bool(true)
rejected
