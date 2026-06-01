<?php

$key = 'A8C_PATCH_CLI_DISABLED';

if (PHP_SAPI !== 'cli') {
    fwrite(STDERR, "expected cli SAPI\n");
    exit(1);
}

if (!function_exists('a8c_sapi_putenv')) {
    fwrite(STDERR, "a8c_sapi_putenv is not registered\n");
    exit(1);
}

putenv($key);

try {
    a8c_sapi_putenv($key . '=visible');
    fwrite(STDERR, "disabled a8c_sapi_putenv did not throw\n");
    exit(1);
} catch (Error $e) {
    if (strpos($e->getMessage(), 'disabled') === false) {
        fwrite(STDERR, "unexpected disabled error: {$e->getMessage()}\n");
        exit(1);
    }
}

if (getenv($key, true) !== false || getenv($key, false) !== false) {
    fwrite(STDERR, "disabled call changed environment\n");
    exit(1);
}

echo json_encode([
    'driver' => 'cli',
    'mode' => 'disabled',
    'status' => 'ok',
], JSON_PRETTY_PRINT), "\n";
