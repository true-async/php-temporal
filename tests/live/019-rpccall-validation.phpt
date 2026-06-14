--TEST--
Core\Connection::rpcCall validates metadata and timeout before hitting the wire
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

await(spawn(function () {
    $conn = new Connection(temporal_test_address());

    // A newline inside a value would corrupt the "key\nvalue" wire form.
    try {
        $conn->rpcCall(1, 'GetSystemInfo', '', 0, ['x-key' => "evil\nvalue"]);
    } catch (ValueError $e) {
        var_dump($e->getMessage());
    }

    // ... and inside a key likewise.
    try {
        $conn->rpcCall(1, 'GetSystemInfo', '', 0, ["evil\nkey" => 'v']);
    } catch (ValueError $e) {
        var_dump($e->getMessage());
    }

    // A negative timeout used to be silently cast to a huge uint32.
    try {
        $conn->rpcCall(1, 'GetSystemInfo', '', -1);
    } catch (ValueError $e) {
        var_dump($e->getMessage());
    }
}));
?>
--EXPECT--
string(59) "Temporal metadata keys and values must not contain newlines"
string(59) "Temporal metadata keys and values must not contain newlines"
string(61) "rpcCall timeout must be between 0 and 4294967295 milliseconds"
