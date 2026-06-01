#!/usr/bin/env bash
set -euo pipefail

LIMA_INSTANCE="${LIMA_INSTANCE:-agc-debian-12-amd64}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/a8c_sapi_putenv_fuzz}"
VERSION="${VERSION:-8.5}"
PORT="${PORT:-9185}"
ITERATIONS="${ITERATIONS:-20000}"
SEED="${SEED:-43148}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

limactl shell "$LIMA_INSTANCE" -- sudo rm -rf "$REMOTE_DIR"
limactl copy --recursive "$repo_root" "$LIMA_INSTANCE:$REMOTE_DIR"

limactl shell "$LIMA_INSTANCE" -- bash -s -- "$REMOTE_DIR" "$VERSION" "$PORT" "$ITERATIONS" "$SEED" <<'REMOTE'
set -euo pipefail

remote_dir="$1"
version="$2"
port="$3"
iterations="$4"
seed="$5"

cd "$remote_dir"
make clean >/dev/null 2>&1 || true
rm -rf autom4te.cache build include modules .libs run-tests.php acinclude.m4 aclocal.m4 \
  config.h config.h.in config.log config.nice config.status configure libtool \
  Makefile Makefile.fragments Makefile.global Makefile.objects
find . -maxdepth 2 \( -name '*.dep' -o -name '*.la' -o -name '*.lo' \) -delete

"phpize${version}" >/dev/null
./configure --enable-a8c-sapi-putenv --with-php-config="/usr/bin/php-config${version}" >/dev/null
make -j"$(nproc)" >/dev/null
"php${version}" run-tests.php -q -d "extension=$remote_dir/modules/a8c_sapi_putenv.so" tests
"php${version}" -d "extension=$remote_dir/modules/a8c_sapi_putenv.so" "$remote_dir/tools/fuzz-cli.php" "$iterations" "$seed"

ext_dir="$("php-config${version}" --extension-dir)"
sudo install -m 0644 "$remote_dir/modules/a8c_sapi_putenv.so" "$ext_dir/a8c_sapi_putenv.so"
echo "extension=a8c_sapi_putenv.so" | sudo tee "/etc/php/${version}/mods-available/a8c_sapi_putenv.ini" >/dev/null
sudo phpenmod -v "$version" -s fpm a8c_sapi_putenv

sudo install -d -m 0755 /var/www/a8c-sapi-putenv-fuzz
sudo cp "$remote_dir"/tools/fpm/www/*.php /var/www/a8c-sapi-putenv-fuzz/
sudo chown -R www-data:www-data /var/www/a8c-sapi-putenv-fuzz

pool="/etc/php/${version}/fpm/pool.d/www.conf"
sudo sed -i \
  -e 's/^;*clear_env = .*/clear_env = no/' \
  -e 's/^;*pm.max_children = .*/pm.max_children = 5/' \
  -e 's/^;*pm.start_servers = .*/pm.start_servers = 2/' \
  -e 's/^;*pm.min_spare_servers = .*/pm.min_spare_servers = 1/' \
  -e 's/^;*pm.max_spare_servers = .*/pm.max_spare_servers = 3/' \
  "$pool"
if grep -q '^php_admin_value\[disable_functions\]' "$pool"; then
  sudo sed -i 's/^php_admin_value\[disable_functions\].*/php_admin_value[disable_functions] = putenv/' "$pool"
else
  printf '\nphp_admin_value[disable_functions] = putenv\n' | sudo tee -a "$pool" >/dev/null
fi

sudo tee "/etc/nginx/sites-available/a8c-sapi-putenv-fuzz-${version}" >/dev/null <<NGINX
server {
    listen 127.0.0.1:${port};
    server_name _;
    root /var/www/a8c-sapi-putenv-fuzz;
    index fuzz.php;

    location ~ \.php$ {
        include snippets/fastcgi-php.conf;
        fastcgi_pass unix:/run/php/php${version}-fpm.sock;
    }
}
NGINX
sudo ln -sf "/etc/nginx/sites-available/a8c-sapi-putenv-fuzz-${version}" "/etc/nginx/sites-enabled/a8c-sapi-putenv-fuzz-${version}"
sudo "php-fpm${version}" -t
sudo systemctl restart "php${version}-fpm"
sudo nginx -t
sudo systemctl restart nginx

curl -fsS "http://127.0.0.1:${port}/fuzz.php?n=${iterations}&seed=${seed}" | tee "$remote_dir/fpm-fuzz-${version}.json"
jq -e '.status == "ok" and .putenv_disabled == true' "$remote_dir/fpm-fuzz-${version}.json" >/dev/null
REMOTE

mkdir -p "$repo_root/docs/verification"
limactl copy "$LIMA_INSTANCE:$REMOTE_DIR/fpm-fuzz-${VERSION}.json" "$repo_root/docs/verification/"
