--TEST--
a8c_sapi_putenv interleaves with putenv on the same key
--SKIPIF--
<?php
if (!extension_loaded('a8c_sapi_putenv')) die('skip extension not loaded');
if (!function_exists('putenv')) die('skip putenv disabled');
?>
--FILE--
<?php
$key = 'A8C_SAPI_MIXED_TEST';

var_dump(putenv($key . '=core-original'));
var_dump(getenv($key, true));

var_dump(a8c_sapi_putenv($key . '=a8c-one'));
var_dump(getenv($key, true));

var_dump(putenv($key . '=core-two'));
var_dump(getenv($key, true));

var_dump(a8c_sapi_putenv($key . '=a8c-three'));
var_dump(getenv($key, true));

var_dump(putenv($key));
var_dump(getenv($key, true));

var_dump(a8c_sapi_putenv($key . '=a8c-final'));
var_dump(getenv($key, true));

var_dump(a8c_sapi_putenv($key));
var_dump(getenv($key, true));
?>
--EXPECT--
bool(true)
string(13) "core-original"
bool(true)
string(7) "a8c-one"
bool(true)
string(8) "core-two"
bool(true)
string(9) "a8c-three"
bool(true)
bool(false)
bool(true)
string(9) "a8c-final"
bool(true)
bool(false)
