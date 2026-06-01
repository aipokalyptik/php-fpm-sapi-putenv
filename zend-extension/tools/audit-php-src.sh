#!/usr/bin/env bash
set -euo pipefail

PHP_SRC_DIR="${PHP_SRC_DIR:-/tmp/a8c_php_src/php-src}"
TAGS=(php-8.0.0 php-8.1.0 php-8.2.0 php-8.3.0 php-8.4.0 php-8.5.0)

if [ ! -d "$PHP_SRC_DIR/.git" ]; then
  mkdir -p "$(dirname "$PHP_SRC_DIR")"
  git clone https://github.com/php/php-src.git "$PHP_SRC_DIR"
fi

git -C "$PHP_SRC_DIR" fetch --tags --quiet

for tag in "${TAGS[@]}"; do
  echo "## ${tag}"
  echo

  echo "### sapi_getenv and getenv implementation"
  git -C "$PHP_SRC_DIR" grep -n \
    -e 'SAPI_API char \*sapi_getenv' \
    -e 'PHP_FUNCTION(getenv)' \
    -e 'php_getenv' \
    "$tag" -- main ext/standard sapi | sed -n '1,80p' || true
  echo

  echo "### FastCGI request environment readers/writers"
  git -C "$PHP_SRC_DIR" grep -n \
    -e 'fcgi_getenv' \
    -e 'fcgi_putenv' \
    -e 'fcgi_loadenv' \
    -e 'sapi_cgibin_getenv' \
    "$tag" -- main sapi | sed -n '1,120p' || true
  echo

  echo "### Superglobal import paths"
  git -C "$PHP_SRC_DIR" grep -n \
    -e 'php_import_environment_variables' \
    -e 'php_load_environment_variables' \
    -e 'TRACK_VARS_ENV' \
    -e 'TRACK_VARS_SERVER' \
    "$tag" -- main ext/standard sapi | sed -n '1,140p' || true
  echo
done
