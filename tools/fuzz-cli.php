<?php

if (!extension_loaded('a8c_sapi_putenv')) {
    fwrite(STDERR, "a8c_sapi_putenv extension is not loaded\n");
    exit(2);
}

$iterations = (int)($argv[1] ?? 10000);
$seed = (int)($argv[2] ?? 0xA8C);
mt_srand($seed);

function random_ascii(int $min, int $max): string
{
    $len = mt_rand($min, $max);
    $out = '';
    for ($i = 0; $i < $len; $i++) {
        $out .= chr(mt_rand(33, 126));
    }
    return str_replace('=', '_', $out);
}

function random_value(int $max): string
{
    $len = mt_rand(0, $max);
    $out = '';
    for ($i = 0; $i < $len; $i++) {
        $out .= chr(mt_rand(32, 126));
    }
    return $out;
}

$invalid = ['', '=bad', "=\0bad"];
foreach ($invalid as $setting) {
    try {
        a8c_sapi_putenv($setting);
        throw new RuntimeException('invalid setting did not throw');
    } catch (ValueError) {
    }
}

for ($i = 0; $i < $iterations; $i++) {
    $key = 'A8C_FUZZ_' . $i . '_' . random_ascii(1, 48);
    $value = random_value(256);
    $setting = $key . '=' . $value;

    if (a8c_sapi_putenv($setting) !== true) {
        throw new RuntimeException("set returned false at iteration {$i}");
    }
    if (getenv($key, true) !== $value || getenv($key, false) !== $value) {
        throw new RuntimeException("set mismatch at iteration {$i}");
    }
    if (a8c_sapi_putenv($key) !== true) {
        throw new RuntimeException("unset returned false at iteration {$i}");
    }
    if (getenv($key, true) !== false || getenv($key, false) !== false) {
        throw new RuntimeException("unset mismatch at iteration {$i}");
    }
}

$nulCases = ["A8C_NUL_A\0B=C", "A8C_NUL_B=C\0D", "A8C_NUL_C\0"];
foreach ($nulCases as $case) {
    try {
        a8c_sapi_putenv($case);
    } catch (Throwable $e) {
        throw new RuntimeException('NUL case threw unexpectedly: ' . $e->getMessage(), 0, $e);
    }
}

echo json_encode([
    'driver' => 'cli',
    'iterations' => $iterations,
    'seed' => $seed,
    'status' => 'ok',
], JSON_PRETTY_PRINT), "\n";
