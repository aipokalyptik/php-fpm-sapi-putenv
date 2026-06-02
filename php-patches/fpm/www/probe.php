<?php

header('Content-Type: application/json');

$keys = ['A8C_PATCH_SECRET_ONE', 'A8C_PATCH_SECRET_TWO', 'A8C_PATCH_KEEP_ME'];
$out = [
    'php_version' => PHP_VERSION,
    'sapi' => PHP_SAPI,
    'function_exists' => function_exists('a8c_sapi_putenv'),
    'putenv_function_exists' => function_exists('putenv'),
    'keys' => [],
];

foreach ($keys as $key) {
    $out['keys'][$key] = [
        'env_superglobal' => $_ENV[$key] ?? false,
        'server_superglobal' => $_SERVER[$key] ?? false,
        'getenv_local' => getenv($key, true),
        'getenv_sapi' => getenv($key, false),
    ];
}

echo json_encode($out, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
