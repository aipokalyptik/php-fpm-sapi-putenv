--TEST--
a8c_sapi_putenv works when putenv is disabled
--SKIPIF--
<?php
if (!extension_loaded('a8c_sapi_putenv')) die('skip extension not loaded');
?>
--INI--
disable_functions=putenv
--FILE--
<?php
var_dump(function_exists('putenv'));
var_dump(a8c_sapi_putenv('A8C_SAPI_DISABLED_TEST=visible'));
var_dump(getenv('A8C_SAPI_DISABLED_TEST', true));
var_dump(getenv('A8C_SAPI_DISABLED_TEST', false));
var_dump(a8c_sapi_putenv('A8C_SAPI_DISABLED_TEST'));
var_dump(getenv('A8C_SAPI_DISABLED_TEST', true));
var_dump(getenv('A8C_SAPI_DISABLED_TEST', false));
?>
--EXPECT--
bool(false)
bool(true)
string(7) "visible"
string(7) "visible"
bool(true)
bool(false)
bool(false)
