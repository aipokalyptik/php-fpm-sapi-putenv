<?php

header('Content-Type: application/json');

$scrubbed = ['A8C_PATCH_SECRET_ONE', 'A8C_PATCH_SECRET_TWO'];
$kept = ['A8C_PATCH_KEEP_ME'];
$keys = array_merge($scrubbed, $kept);

function snapshot(array $keys): array
{
    $out = [];
    foreach ($keys as $key) {
        $out[$key] = [
            'env_superglobal' => $_ENV[$key] ?? false,
            'server_superglobal' => $_SERVER[$key] ?? false,
            'getenv_local' => getenv($key, true),
            'getenv_sapi' => getenv($key, false),
        ];
    }
    return $out;
}

$before = snapshot($keys);

foreach ($scrubbed as $key) {
    unset($_ENV[$key], $_SERVER[$key]);
    if (a8c_sapi_putenv($key) !== true) {
        throw new RuntimeException("a8c_sapi_putenv failed for {$key}");
    }
}

$after = snapshot($keys);

echo json_encode([
    'php_version' => PHP_VERSION,
    'sapi' => PHP_SAPI,
    'function_exists' => function_exists('a8c_sapi_putenv'),
    'before' => $before,
    'after' => $after,
], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
