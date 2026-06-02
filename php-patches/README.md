# PHP Patch Implementation

These patches add a built-in `a8c_sapi_putenv(string $assignment): bool`
function guarded by:

```ini
a8c_sapi_putenv.enable = 0
```

The setting is `PHP_INI_SYSTEM` and defaults to disabled. Enable it from system
configuration or an fpm pool, for example:

```ini
php_admin_value[a8c_sapi_putenv.enable] = 1
```

When enabled under php-fpm, `a8c_sapi_putenv()` mirrors `putenv()`
process-environment bookkeeping and also updates the current FastCGI request
environment through a narrow SAPI hook. The php-fpm SAPI implements that hook
with its internal `fcgi_putenv()`.

When enabled under a SAPI that does not provide the hook, such as CLI, the
function returns `false` and does not change the process environment. When the
INI setting is disabled, the function throws an `Error`.

Patch files live in `php-patches/patches/` and are generated/tested against PHP
8.0, 8.1, 8.2, 8.3, 8.4, and 8.5.

Regenerate patches with:

```sh
php-patches/tools/generate-patches.py --src-root /tmp/a8c_php_patch_refs
```

Run the Debian amd64 Lima build and FPM/nginx validation with:

```sh
php-patches/tools/lima-build-test.sh
```

The FPM validation pool sets `disable_functions=putenv` and asserts that
userland `putenv()` is unavailable while `a8c_sapi_putenv()` still scrubs the
FastCGI request environment.
