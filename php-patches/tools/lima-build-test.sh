#!/usr/bin/env bash
set -euo pipefail

VM=${VM:-agc-debian-12-amd64}
VERSIONS=${VERSIONS:-"8.0 8.1 8.2 8.3 8.4 8.5"}
REMOTE_REPO=${REMOTE_REPO:-/tmp/a8c_sapi_putenv}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

tar --exclude=.git -C "$(dirname "$ROOT")" -czf /tmp/a8c_sapi_putenv-current.tgz "$(basename "$ROOT")"
limactl copy /tmp/a8c_sapi_putenv-current.tgz "$VM:/tmp/a8c_sapi_putenv-current.tgz"

limactl shell "$VM" -- bash -s -- "$REMOTE_REPO" "$VERSIONS" <<'REMOTE'
set -euo pipefail

repo=$1
versions=$2

sudo apt-get update -qq
sudo apt-get install -y -qq git build-essential autoconf bison re2c pkg-config \
    libxml2-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev libonig-dev \
    libreadline-dev nginx curl ca-certificates jq >/tmp/a8c-apt.log

rm -rf "$repo"
tar -xzf /tmp/a8c_sapi_putenv-current.tgz -C /tmp

base=/tmp/a8c-php-patches
mkdir -p "$base"/{src,logs,run,results}

next_port() {
    php -r 'echo random_int(20000, 50000), "\n";'
}

stop_services() {
    local run_dir=$1
    if [ -f "$run_dir/nginx.pid" ]; then
        nginx -p "$run_dir/nginx" -c "$run_dir/nginx.conf" -s quit >/dev/null 2>&1 || true
    fi
    if [ -f "$run_dir/php-fpm.pid" ]; then
        kill "$(cat "$run_dir/php-fpm.pid")" >/dev/null 2>&1 || true
    fi
}

build_one() {
    local version=$1
    local tag="PHP-${version}.0"
    local src="$base/src/php-$version"
    local prefix="$base/install/php-$version"
    local run_dir="$base/run/php-$version"
    local log_dir="$base/logs/php-$version"
    local result_dir="$base/results/php-$version"
    local fpm_port http_port php_bin

    rm -rf "$src" "$prefix" "$run_dir" "$log_dir" "$result_dir"
    mkdir -p "$run_dir/nginx/logs" "$log_dir" "$result_dir"
    php_bin=$(command -v "php$version" || command -v php)

    git clone --depth 1 --branch "$tag" https://github.com/php/php-src.git "$src" >"$log_dir/clone.log" 2>&1
    (cd "$src" && git apply "$repo/php-patches/patches/php-$version.patch")

    (cd "$src" && PHP="$php_bin" ./buildconf --force) >"$log_dir/buildconf.log" 2>&1
    (cd "$src" && PHP="$php_bin" CFLAGS="-O0 -g0" ./configure \
        --prefix="$prefix" \
        --disable-all \
        --enable-cli \
        --enable-fpm \
        --without-pear) >"$log_dir/configure.log" 2>&1
    (cd "$src" && PHP="$php_bin" make -j"$(nproc)" sapi/cli/php sapi/fpm/php-fpm) >"$log_dir/make.log" 2>&1

    "$src/sapi/cli/php" "$repo/php-patches/tests/cli-disabled.php" >"$result_dir/cli-disabled.json"
    "$src/sapi/cli/php" -d a8c_sapi_putenv.enable=1 "$repo/php-patches/tests/cli-enabled-noop.php" >"$result_dir/cli-enabled-noop.json"

    fpm_port=$(next_port)
    http_port=$(next_port)

    cat >"$run_dir/php-fpm.conf" <<EOF
[global]
pid = $run_dir/php-fpm.pid
error_log = $log_dir/php-fpm.log
daemonize = yes

[www]
listen = 127.0.0.1:$fpm_port
pm = static
pm.max_children = 2
clear_env = no
php_admin_value[a8c_sapi_putenv.enable] = 1
php_admin_value[disable_functions] = putenv
php_admin_value[variables_order] = EGPCS
EOF

    cat >"$run_dir/nginx.conf" <<EOF
daemon on;
pid $run_dir/nginx.pid;
error_log $log_dir/nginx-error.log;
events { worker_connections 64; }
http {
    access_log $log_dir/nginx-access.log;
    server {
        listen 127.0.0.1:$http_port;
        root $repo/php-patches/fpm/www;
        location / {
            try_files \$uri =404;
        }
        location ~ \.php$ {
            fastcgi_pass 127.0.0.1:$fpm_port;
            fastcgi_param SCRIPT_FILENAME \$document_root\$fastcgi_script_name;
            fastcgi_param SCRIPT_NAME \$fastcgi_script_name;
            fastcgi_param REQUEST_METHOD \$request_method;
            fastcgi_param QUERY_STRING \$query_string;
            fastcgi_param CONTENT_TYPE \$content_type;
            fastcgi_param CONTENT_LENGTH \$content_length;
            fastcgi_param A8C_PATCH_SECRET_ONE secret-one;
            fastcgi_param A8C_PATCH_SECRET_TWO secret-two;
            fastcgi_param A8C_PATCH_KEEP_ME keep-me;
            fastcgi_param A8C_PATCH_FUZZ_TARGET initial;
        }
    }
}
EOF

    trap 'stop_services "$run_dir"' RETURN
    "$src/sapi/fpm/php-fpm" -y "$run_dir/php-fpm.conf" -p "$run_dir"
    nginx -p "$run_dir/nginx" -c "$run_dir/nginx.conf"

    curl -fsS "http://127.0.0.1:$http_port/probe.php" >"$result_dir/probe.json"
    curl -fsS "http://127.0.0.1:$http_port/scrub.php" >"$result_dir/scrub.json"
    curl -fsS "http://127.0.0.1:$http_port/fuzz.php?n=2000&seed=2764" >"$result_dir/fuzz.json"
    "$src/sapi/cli/php" "$repo/php-patches/tests/assert-fpm.php" \
        "$result_dir/probe.json" "$result_dir/scrub.json" "$result_dir/fuzz.json" \
        >"$result_dir/fpm-assert.json"
    stop_services "$run_dir"
    trap - RETURN

    jq -n \
        --arg version "$version" \
        --slurpfile cli_disabled "$result_dir/cli-disabled.json" \
        --slurpfile cli_enabled_noop "$result_dir/cli-enabled-noop.json" \
        --slurpfile fpm "$result_dir/fpm-assert.json" \
        '{version:$version,status:"ok",cli_disabled:$cli_disabled[0],cli_enabled_noop:$cli_enabled_noop[0],fpm:$fpm[0]}' \
        >"$result_dir/summary.json"
    cat "$result_dir/summary.json"
}

for version in $versions; do
    build_one "$version"
done
REMOTE
