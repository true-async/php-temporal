--TEST--
Connecting to a dead port throws ConnectionException through the async bridge
--EXTENSIONS--
temporal
--FILE--
<?php
use TrueAsync\Temporal\Core\Connection;
use TrueAsync\Temporal\ConnectionException;
use function Async\spawn;
use function Async\await;

$res = await(spawn(function () {
    try {
        new Connection('127.0.0.1:1');
        return "no exception";
    } catch (ConnectionException $e) {
        return "ConnectionException";
    } catch (\Throwable $e) {
        return "other: " . get_class($e);
    }
}));
echo $res, "\n";
?>
--EXPECT--
ConnectionException
