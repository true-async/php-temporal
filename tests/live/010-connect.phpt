--TEST--
Core\Connection connects to a live Temporal frontend
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

$ok = await(spawn(fn() => new Connection(temporal_test_address()) instanceof Connection));
var_dump($ok);
?>
--EXPECT--
bool(true)
