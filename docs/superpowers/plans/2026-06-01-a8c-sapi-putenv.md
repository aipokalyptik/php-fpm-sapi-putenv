# a8c_sapi_putenv Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and verify a PHP extension that provides `a8c_sapi_putenv()` for scrubbing php-fpm FastCGI request variables from the SAPI getenv path.

**Architecture:** The extension exposes one PHP function and keeps the reusable logic in `src/a8c_sapi_env.c`. It owns process-environment mutation while recording request shutdown state in PHP standard's `BG(putenv_ht)`, then updates existing php-fpm FastCGI request entries by walking the request entry list without computing php-fpm's private hash algorithm.

**Tech Stack:** PHP 8.0-8.5 phpize extension, Zend Engine C APIs, php-fpm, nginx, Debian amd64 Lima.

---

### Task 1: Extension Contract

**Files:**
- Create: `config.m4`
- Create: `php_a8c_sapi_putenv.h`
- Create: `a8c_sapi_putenv.c`
- Create: `src/a8c_sapi_env.h`
- Create: `src/a8c_sapi_env.c`
- Test: `tests/001-basic.phpt`
- Test: `tests/002-invalid.phpt`

- [x] **Step 1: Write failing tests**

Create PHPTs that expect the function to exist, return `true` on set/unset, update normal getenv values, and throw on empty or leading-`=` settings.

- [x] **Step 2: Verify red state**

Run: `php run-tests.php tests/001-basic.phpt tests/002-invalid.phpt`

Expected before implementation: FAIL because `a8c_sapi_putenv()` is undefined.

- [x] **Step 3: Implement minimal function**

Expose `a8c_sapi_putenv(string $assignment): bool` and parse the key/value in a helper. Empty strings and strings starting with `=` raise `ValueError`, matching `putenv()`.

- [x] **Step 4: Run tests**

Run: `phpize && ./configure && make && php run-tests.php -d extension=modules/a8c_sapi_putenv.so tests`

Expected after implementation: PASS on CLI for the normal putenv-compatible contract.

### Task 2: php-fpm Verification Harness

**Files:**
- Create: `tools/lima-verify.sh`
- Create: `tools/fpm/www/probe.php`
- Create: `tools/fpm/www/scrub.php`

- [x] **Step 1: Install/build prerequisites in Debian amd64 Lima**

Use a Debian amd64 Lima VM and install nginx plus PHP CLI/FPM/dev packages for PHP 8.0 through 8.5.

- [x] **Step 2: Build extension per PHP version**

For each `phpizeX.Y`, run `phpize`, `./configure --with-php-config=php-configX.Y`, and `make`.

- [x] **Step 3: Configure php-fpm and nginx**

Configure nginx with custom `fastcgi_param` values including `A8C_SECRET_ONE`, `A8C_SECRET_TWO`, and `A8C_KEEP_ME`.

- [x] **Step 4: Prove baseline problem**

Request `/probe.php` and assert the custom values appear in `$_ENV`, `$_SERVER`, `getenv($key, true)`, and `getenv($key, false)` before scrubbing.

- [x] **Step 5: Prove extension behavior**

Request `/scrub.php` and assert scrubbed keys are gone from `getenv($key, false)` and normal env, while `A8C_KEEP_ME` remains visible.

### Task 3: PHP Source Audit

**Files:**
- Create: `tools/audit-php-src.sh`
- Create: `docs/source-audit.md`

- [x] **Step 1: Check out php-src**

Clone `https://github.com/php/php-src.git` and inspect tags `php-8.0.0`, `php-8.1.0`, `php-8.2.0`, `php-8.3.0`, `php-8.4.0`, and `php-8.5.0`.

- [x] **Step 2: Search read paths**

Search for SAPI/FastCGI environment readers, standard environment functions, superglobal import paths, `fcgi_getenv`, `sapi_getenv`, `php_getenv`, and `fcgi_loadenv`.

- [x] **Step 3: Document findings**

Record whether any built-in functions or superglobals can read the scrubbed FastCGI values after the request hash and process environment are cleared.

### Task 4: Documentation

**Files:**
- Create: `README.md`
- Create: `docs/drop-in.md`

- [x] **Step 1: Document installation and usage**

Include phpize build commands and php-fpm loading instructions.

- [x] **Step 2: Document drop-in extraction**

Explain exactly which files/functions to copy into another extension and how to register the function.
