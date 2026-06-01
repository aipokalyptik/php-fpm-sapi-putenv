--TEST--
a8c_sapi_putenv rejects invalid syntax like putenv
--SKIPIF--
<?php
if (!extension_loaded('a8c_sapi_putenv')) die('skip extension not loaded');
?>
--FILE--
<?php
foreach (['', '=bad'] as $value) {
    try {
        a8c_sapi_putenv($value);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
?>
--EXPECT--
a8c_sapi_putenv(): Argument #1 ($assignment) must have a valid syntax
a8c_sapi_putenv(): Argument #1 ($assignment) must have a valid syntax
