# Code and Security Review

Date: 2026-06-01

Scope:

- C extension entrypoint and helper code
- PHPT tests
- nginx/php-fpm verification harness
- php-src source-audit notes
- CLI and fpm fuzz drivers

## Summary

No memory-safety crash, secret-retention bypass, or unintended key deletion was found in the tested PHP 8.0-8.5 matrix.

Implementation issues fixed during review:

- The return value from the php-fpm FastCGI request hash update is now propagated. Before that change, an allocation failure or unexpected FastCGI request state could have returned `true` after updating only the process environment.
- `a8c_sapi_putenv()` no longer depends on PHP's registered `putenv()` function, so it still works when `disable_functions=putenv` is configured.
- The process-environment bookkeeping now writes to PHP standard's existing `BG(putenv_ht)` table instead of a parallel extension-owned restore table. This preserves core `putenv()` interleaving semantics and removes the ZTS module-globals requirement.

## Findings

### Fixed: `disable_functions=putenv` disabled `a8c_sapi_putenv`

`a8c_sapi_putenv` previously delegated process-environment mutation to PHP's registered `putenv()` function. If `putenv` was disabled via `disable_functions`, PHP removed it from the function table. In that configuration, `a8c_sapi_putenv()` threw an `Error` when it tried to call `putenv`.

Evidence:

```bash
php -d disable_functions=putenv -d extension=modules/a8c_sapi_putenv.so \
  -r 'var_dump(function_exists("putenv"), a8c_sapi_putenv("A8C_DISABLED_TEST=x"));'
```

Result:

```text
bool(false)
Fatal error: Uncaught Error: Invalid callback putenv, function "putenv" not found or invalid function name
```

Fix:

The extension now owns its process-environment update path instead of calling the registered `putenv()` function, but it records request-shutdown restore state in PHP standard's `BG(putenv_ht)`. Regression coverage is in `tests/003-disable-functions.phpt`, and fpm fuzzing configures `php_admin_value[disable_functions] = putenv`.

### Fixed: parallel restore table interaction with core `putenv()`

An intermediate implementation kept a separate extension-owned restore table. That created a lifetime hazard when user code interleaved PHP core `putenv()` with `a8c_sapi_putenv()` on the same key, because the two request-shutdown destructors could restore pointers owned by the other table.

Fix:

`a8c_sapi_putenv()` now mirrors PHP core's process-environment mutation logic directly into `BG(putenv_ht)`. This gives both APIs one shared del-before-add table, so interleaving `putenv()` and `a8c_sapi_putenv()` on the same key uses the same request-shutdown ownership model as core. Coverage is in `tests/004-mixed-putenv.phpt` and the mixed-API section of `tools/fuzz-cli.php`.

### Fixed: ZTS module globals registration

The same intermediate implementation declared extension module globals for its parallel restore table without registering them for ZTS builds.

Fix:

The extension no longer declares module globals. It uses PHP standard's already-registered `BG(putenv_ht)`, so there is no extension-local globals allocation path to maintain for ZTS.

## Fuzz Coverage

Added:

- `tools/fuzz-cli.php`
- `tools/fpm/www/fuzz.php`
- `tools/lima-fuzz.sh`

Covered cases:

- invalid settings: empty string and leading `=`
- randomized valid names and values in CLI
- randomized values for an existing FastCGI request key in fpm
- repeated set/get/unset cycles
- process environment and SAPI environment agreement
- retained non-target keys
- mixed `putenv()` and `a8c_sapi_putenv()` operations on the same key
- embedded NUL smoke cases
- php-fpm FastCGI request hash set/delete path via nginx

Commands run:

```bash
phpize >/dev/null
./configure --enable-a8c-sapi-putenv >/dev/null
make -j$(sysctl -n hw.ncpu) CFLAGS='-g -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers'
php run-tests.php -q -d extension=$(pwd)/modules/a8c_sapi_putenv.so tests
php -d extension=$(pwd)/modules/a8c_sapi_putenv.so tools/fuzz-cli.php 20000 43148
ITERATIONS=20000 SEED=43148 VERSION=8.5 tools/lima-fuzz.sh
tools/lima-verify.sh
```

Results:

- Local PHP 8.5 PHPTs: 4/4 passed
- Local PHP 8.5 CLI fuzz: 20,000 iterations passed
- Debian amd64 PHP 8.5 fpm fuzz with `putenv` disabled: 20,000 iterations passed
- Debian amd64 PHP 8.0-8.5 matrix: baseline and scrub verification passed for every version

The fpm fuzz artifact is `docs/verification/fpm-fuzz-8.5.json`.

## Static Review Notes

Compiler diagnostics:

- Built locally with `-Wall -Wextra -Werror`
- `clang --analyze` completed without diagnostics

Unavailable on the host:

- `cppcheck`
- `scan-build`
- `valgrind`

Memory-safety review:

- The FastCGI fallback duplicates PHP's 8.0-8.5 private request-hash layout enough to walk the existing request entry list, but it does not duplicate or compute PHP's FastCGI hash algorithm.
- The fallback is restricted to `fpm-fcgi` and `cgi-fcgi` and requires a non-null `SG(server_context)`.
- Fallback deletion marks matching existing entries with `val = NULL`, which makes `fcgi_hash_get()` return false for the key and makes `fcgi_hash_apply()` style iteration skip the entry.
- Existing-key updates allocate through `malloc()` because PHP's FastCGI hash storage is cleaned by PHP core with `free()`.
- The public API is one string argument, parsed with Zend parameter parsing.

Residual risk:

- The private FastCGI layout is not a public ABI. It matched the audited PHP 8.0-8.5 source and passed packaged Debian/Sury fpm runtime testing, but a future PHP minor or downstream distro patch could drift.
- The FastCGI request update path can only update or delete FastCGI params already present in the request. It intentionally does not create brand-new FastCGI request keys, because doing that would require duplicating php-fpm's private hash algorithm.
- Embedded-NUL input does not crash in fuzzing, but PHP's own `putenv()` semantics around embedded NUL bytes are not useful for real environment names. Operational callers should use normal environment key syntax.
