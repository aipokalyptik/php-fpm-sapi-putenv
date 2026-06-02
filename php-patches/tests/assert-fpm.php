<?php

if ($argc !== 4) {
    fwrite(STDERR, "usage: php assert-fpm.php probe.json scrub.json fuzz.json\n");
    exit(1);
}

function load_json(string $path): array
{
    $data = json_decode(file_get_contents($path), true);
    if (!is_array($data)) {
        fwrite(STDERR, "invalid json: {$path}\n");
        exit(1);
    }
    return $data;
}

function fail(string $message): void
{
    fwrite(STDERR, $message . "\n");
    exit(1);
}

$probe = load_json($argv[1]);
$scrub = load_json($argv[2]);
$fuzz = load_json($argv[3]);

if (($probe['sapi'] ?? null) !== 'fpm-fcgi' || !($probe['function_exists'] ?? false)) {
    fail('probe did not run under fpm with a8c_sapi_putenv registered');
}
if (($probe['putenv_function_exists'] ?? true) !== false) {
    fail('fpm test did not disable userland putenv');
}

$expected = [
    'A8C_PATCH_SECRET_ONE' => 'secret-one',
    'A8C_PATCH_SECRET_TWO' => 'secret-two',
    'A8C_PATCH_KEEP_ME' => 'keep-me',
];

foreach ($expected as $key => $value) {
    foreach (['env_superglobal', 'server_superglobal', 'getenv_sapi'] as $field) {
        if (($probe['keys'][$key][$field] ?? null) !== $value) {
            fail("probe mismatch for {$key} {$field}");
        }
    }
    if (($probe['keys'][$key]['getenv_local'] ?? null) !== false) {
        fail("probe unexpectedly populated local process env for {$key}");
    }
}

foreach (['A8C_PATCH_SECRET_ONE', 'A8C_PATCH_SECRET_TWO'] as $key) {
    foreach (['env_superglobal', 'server_superglobal', 'getenv_sapi'] as $field) {
        if (($scrub['before'][$key][$field] ?? null) !== $expected[$key]) {
            fail("scrub before mismatch for {$key} {$field}");
        }
    }
    if (($scrub['before'][$key]['getenv_local'] ?? null) !== false) {
        fail("scrub before unexpectedly populated local process env for {$key}");
    }
    foreach (['env_superglobal', 'server_superglobal', 'getenv_local', 'getenv_sapi'] as $field) {
        if (($scrub['after'][$key][$field] ?? null) !== false) {
            fail("scrub after still exposes {$key} {$field}");
        }
    }
}

foreach (['env_superglobal', 'server_superglobal', 'getenv_sapi'] as $field) {
    if (($scrub['after']['A8C_PATCH_KEEP_ME'][$field] ?? null) !== 'keep-me') {
        fail("scrub changed keep value {$field}");
    }
}
if (($scrub['after']['A8C_PATCH_KEEP_ME']['getenv_local'] ?? null) !== false) {
    fail('scrub unexpectedly populated local process env for keep value');
}

if (($fuzz['status'] ?? null) !== 'ok' || ($fuzz['sapi'] ?? null) !== 'fpm-fcgi') {
    fail('fuzz did not pass under fpm');
}

echo json_encode([
    'driver' => 'fpm',
    'status' => 'ok',
    'php_version' => $probe['php_version'] ?? 'unknown',
], JSON_PRETTY_PRINT), "\n";
