# a8c_sapi_putenv

`a8c_sapi_putenv` is a small PHP extension for php-fpm deployments that need to scrub FastCGI parameters before untrusted PHP code can read them.

php-fpm FastCGI params can be visible through:

- `$_ENV`
- `$_SERVER`
- the process environment read by `getenv($key, true)`
- the SAPI environment read first by `getenv($key, false)`

PHP's built-in `putenv('NAME')` clears the process environment path, but it does not remove the FastCGI request hash used by php-fpm's SAPI `getenv` callback. This extension adds:

```php
a8c_sapi_putenv(string $assignment): bool
```

It accepts the same string form as `putenv()`:

```php
a8c_sapi_putenv('NAME=value'); // set process env and php-fpm SAPI env
a8c_sapi_putenv('NAME');       // unset process env and php-fpm SAPI env
```

For a full current-request scrub:

```php
unset($_ENV['A8C_SECRET'], $_SERVER['A8C_SECRET']);
a8c_sapi_putenv('A8C_SECRET');
```

## Build

```bash
phpize
./configure --enable-a8c-sapi-putenv
make
make test TESTS="-d extension=$(pwd)/modules/a8c_sapi_putenv.so tests"
```

Install the built module into the target PHP extension directory and load it from the php-fpm SAPI:

```bash
php-config --extension-dir
sudo install -m 0644 modules/a8c_sapi_putenv.so "$(php-config --extension-dir)/a8c_sapi_putenv.so"
echo 'extension=a8c_sapi_putenv.so' | sudo tee /etc/php/8.5/mods-available/a8c_sapi_putenv.ini
sudo phpenmod -v 8.5 -s fpm a8c_sapi_putenv
sudo systemctl restart php8.5-fpm
```

## Multi-Version Lima Verification

The provided harness uses the Debian amd64 Lima instance named `agc-debian-12-amd64` by default:

```bash
chmod +x tools/lima-verify.sh tools/audit-php-src.sh
tools/lima-verify.sh
```

It builds and tests the extension for PHP 8.0, 8.1, 8.2, 8.3, 8.4, and 8.5, then configures nginx plus php-fpm with custom `fastcgi_param` values and matching fpm pool `env[...]` values to prove all four baseline visibility paths and the scrubbed behavior.

## Source Audit

```bash
chmod +x tools/audit-php-src.sh
PHP_SRC_DIR=/tmp/a8c_php_src/php-src tools/audit-php-src.sh > docs/source-audit.raw.md
```

The summarized audit is in `docs/source-audit.md`.

## Drop-In Use

See `docs/drop-in.md` for the exact files and registration snippet needed to copy this implementation into another extension.
