<?php

header('Content-Type: application/json');

if (!extension_loaded('a8c_sapi_putenv')) {
    http_response_code(500);
    echo json_encode(['status' => 'error', 'error' => 'extension not loaded']), "\n";
    exit;
}

$putenvDisabled = !function_exists('putenv');
$iterations = max(1, min((int)($_GET['n'] ?? 2000), 50000));
$seed = (int)($_GET['seed'] ?? 0xA8C);
mt_srand($seed);

function fuzz_ascii(int $min, int $max): string
{
    $len = mt_rand($min, $max);
    $out = '';
    for ($i = 0; $i < $len; $i++) {
        $out .= chr(mt_rand(65, 90));
    }
    return $out;
}

function fuzz_value(int $max): string
{
    $len = mt_rand(0, $max);
    $out = '';
    for ($i = 0; $i < $len; $i++) {
        $out .= chr(mt_rand(33, 126));
    }
    return $out;
}

try {
    foreach (['', '=bad'] as $setting) {
        try {
            a8c_sapi_putenv($setting);
            throw new RuntimeException('invalid setting did not throw');
        } catch (ValueError) {
        }
    }

    $key = 'A8C_FPM_FUZZ_TARGET';
    if (getenv($key, false) !== 'initial') {
        throw new RuntimeException('required FastCGI fuzz target is missing');
    }

    for ($i = 0; $i < $iterations; $i++) {
        $value = fuzz_value(512);

        if (a8c_sapi_putenv($key . '=' . $value) !== true) {
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

    foreach (["A8C_FPM_NUL_A\0B=C", "A8C_FPM_NUL_B=C\0D"] as $case) {
        a8c_sapi_putenv($case);
    }

    echo json_encode([
        'driver' => 'fpm',
        'php_version' => PHP_VERSION,
        'sapi' => PHP_SAPI,
        'putenv_disabled' => $putenvDisabled,
        'iterations' => $iterations,
        'seed' => $seed,
        'status' => 'ok',
    ], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode([
        'status' => 'error',
        'error_class' => get_class($e),
        'error' => $e->getMessage(),
    ], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
}
