--TEST--
Transport classes and exception hierarchy are registered
--EXTENSIONS--
temporal
--FILE--
<?php
use TrueAsync\Temporal\Core\Connection;
use TrueAsync\Temporal\TemporalException;
use TrueAsync\Temporal\ConnectionException;
use TrueAsync\Temporal\ServiceException;

var_dump(class_exists(Connection::class));
var_dump(is_subclass_of(TemporalException::class, RuntimeException::class));
var_dump(is_subclass_of(ConnectionException::class, TemporalException::class));
var_dump(is_subclass_of(ServiceException::class, TemporalException::class));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
