<?php

$key = 'A8C_PATCH_CLI_NOOP';

if (PHP_SAPI !== 'cli') {
    fwrite(STDERR, "expected cli SAPI\n");
    exit(1);
}

if (!function_exists('a8c_sapi_putenv')) {
    fwrite(STDERR, "a8c_sapi_putenv is not registered\n");
    exit(1);
}

putenv($key);

if (a8c_sapi_putenv($key . '=visible') !== false) {
    fwrite(STDERR, "enabled CLI call unexpectedly did work\n");
    exit(1);
}

if (getenv($key, true) !== false || getenv($key, false) !== false) {
    fwrite(STDERR, "enabled CLI call changed environment\n");
    exit(1);
}

echo json_encode([
    'driver' => 'cli',
    'mode' => 'enabled-noop',
    'status' => 'ok',
], JSON_PRETTY_PRINT), "\n";
