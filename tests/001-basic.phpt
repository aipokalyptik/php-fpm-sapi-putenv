--TEST--
a8c_sapi_putenv mirrors putenv for normal environment values
--SKIPIF--
<?php
if (!extension_loaded('a8c_sapi_putenv')) die('skip extension not loaded');
?>
--FILE--
<?php
var_dump(function_exists('a8c_sapi_putenv'));
var_dump(a8c_sapi_putenv('A8C_SAPI_PUTENV_TEST=visible'));
var_dump(getenv('A8C_SAPI_PUTENV_TEST', true));
var_dump(getenv('A8C_SAPI_PUTENV_TEST', false));
var_dump(a8c_sapi_putenv('A8C_SAPI_PUTENV_TEST'));
var_dump(getenv('A8C_SAPI_PUTENV_TEST', true));
var_dump(getenv('A8C_SAPI_PUTENV_TEST', false));
?>
--EXPECT--
bool(true)
bool(true)
string(7) "visible"
string(7) "visible"
bool(true)
bool(false)
bool(false)
