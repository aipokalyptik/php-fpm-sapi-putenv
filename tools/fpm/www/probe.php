<?php

header('Content-Type: application/json');

$keys = ['A8C_SECRET_ONE', 'A8C_SECRET_TWO', 'A8C_KEEP_ME'];
$out = [
    'php_version' => PHP_VERSION,
    'sapi' => PHP_SAPI,
    'extension_loaded' => extension_loaded('a8c_sapi_putenv'),
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
