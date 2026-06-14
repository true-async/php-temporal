--TEST--
Extension loads and reports its version
--EXTENSIONS--
temporal
--FILE--
<?php
var_dump(extension_loaded('temporal'));
var_dump(phpversion('temporal'));
?>
--EXPECT--
bool(true)
string(9) "0.1.0-dev"
