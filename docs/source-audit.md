# PHP Source Audit

This audit covers php-src tags `php-8.0.0`, `php-8.1.0`, `php-8.2.0`, `php-8.3.0`, `php-8.4.0`, and `php-8.5.0`.

Run:

```bash
PHP_SRC_DIR=/tmp/a8c_php_src/php-src tools/audit-php-src.sh > docs/source-audit.raw.md
```

## Findings

Across the inspected versions, `getenv($name, false)` checks the active SAPI first by calling `sapi_getenv()`. In php-fpm, the SAPI `getenv` callback is `sapi_cgibin_getenv()`, which reads the current FastCGI request environment with `fcgi_getenv()`.

The standard `getenv($name, true)` path reads the process environment through `php_getenv()`. PHP's built-in `putenv()` changes that process environment and tracks request-local restoration in standard module globals.

`$_ENV` and `$_SERVER` are populated during request variable registration. They are ordinary superglobal arrays after population, so userland `unset($_ENV[$key], $_SERVER[$key])` removes those copies for the current request. The extension intentionally does not mutate superglobals because the problem statement already separates those arrays from the SAPI getenv storage.

The only userland built-in read path found for the php-fpm FastCGI request hash is the SAPI getenv callback used by `getenv($name, false)` and environment-import code used to populate superglobals. FPM internals can also read the same request hash for server behavior such as access-log formatting, but those reads use `fcgi_getenv()` against the same request hash entry. After the request hash entry is removed and the process environment entry is removed, the audited PHP core userland paths do not expose another built-in function, define, variable, or superglobal that can read that scrubbed FastCGI value.

## Operational Notes

Call order matters. Scrubbing should happen before untrusted code can call `getenv()` or inspect `$_ENV`/`$_SERVER`.

For a full four-place scrub in php-fpm userland:

```php
unset($_ENV['A8C_SECRET'], $_SERVER['A8C_SECRET']);
a8c_sapi_putenv('A8C_SECRET');
```

`a8c_sapi_putenv('NAME=value')` preserves `putenv()` semantics and also updates the FastCGI request hash when php-fpm exposes `fcgi_putenv`. `a8c_sapi_putenv('NAME')` unsets the process environment entry and deletes the FastCGI request hash entry.

The Debian amd64 verification showed that packaged `php-fpm` binaries do not necessarily export `fcgi_putenv`. The extension therefore includes a local fallback that walks php-fpm's existing FastCGI request entry list and updates or deletes matching entries without recomputing php-fpm's private hash.
