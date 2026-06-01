#!/usr/bin/env bash
set -euo pipefail

LIMA_INSTANCE="${LIMA_INSTANCE:-agc-debian-12-amd64}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/a8c_sapi_putenv}"
VERSIONS=(8.0 8.1 8.2 8.3 8.4 8.5)

log() {
  printf '\n==> %s\n' "$*"
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log "Copying extension source to ${LIMA_INSTANCE}:${REMOTE_DIR}"
limactl shell "$LIMA_INSTANCE" -- sudo rm -rf "$REMOTE_DIR"
limactl copy --recursive "$repo_root" "$LIMA_INSTANCE:$REMOTE_DIR"

log "Installing nginx and PHP packages"
limactl shell "$LIMA_INSTANCE" -- bash -s -- "$REMOTE_DIR" "${VERSIONS[@]}" <<'REMOTE'
set -euo pipefail
remote_dir="$1"
shift
versions=("$@")

export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg lsb-release apt-transport-https jq build-essential autoconf pkg-config nginx

if [ ! -f /etc/apt/sources.list.d/php-sury.list ]; then
  curl -fsSL https://packages.sury.org/php/apt.gpg | sudo gpg --dearmor -o /usr/share/keyrings/deb.sury.org-php.gpg
  echo "deb [signed-by=/usr/share/keyrings/deb.sury.org-php.gpg] https://packages.sury.org/php/ $(lsb_release -sc) main" | sudo tee /etc/apt/sources.list.d/php-sury.list >/dev/null
  sudo apt-get update
fi

packages=()
for version in "${versions[@]}"; do
  packages+=("php${version}-cli" "php${version}-fpm" "php${version}-dev")
done
sudo apt-get install -y "${packages[@]}"

sudo install -d -m 0755 /var/www/a8c-sapi-putenv
sudo cp "$remote_dir"/tools/fpm/www/*.php /var/www/a8c-sapi-putenv/
sudo chown -R www-data:www-data /var/www/a8c-sapi-putenv
REMOTE

for version in "${VERSIONS[@]}"; do
  port="90${version/./}"
  log "Building and testing extension for PHP ${version}"
  limactl shell "$LIMA_INSTANCE" -- bash -s -- "$REMOTE_DIR" "$version" "$port" <<'REMOTE'
set -euo pipefail
remote_dir="$1"
version="$2"
port="$3"

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

ext_dir="$("php-config${version}" --extension-dir)"
sudo install -m 0644 "$remote_dir/modules/a8c_sapi_putenv.so" "$ext_dir/a8c_sapi_putenv.so"
echo "extension=a8c_sapi_putenv.so" | sudo tee "/etc/php/${version}/mods-available/a8c_sapi_putenv.ini" >/dev/null
sudo phpenmod -v "$version" -s fpm a8c_sapi_putenv

pool="/etc/php/${version}/fpm/pool.d/www.conf"
sudo sed -i \
  -e 's/^;*clear_env = .*/clear_env = no/' \
  -e 's/^;*pm.max_children = .*/pm.max_children = 5/' \
  -e 's/^;*pm.start_servers = .*/pm.start_servers = 2/' \
  -e 's/^;*pm.min_spare_servers = .*/pm.min_spare_servers = 1/' \
  -e 's/^;*pm.max_spare_servers = .*/pm.max_spare_servers = 3/' \
  "$pool"
if ! grep -q 'php_admin_value\[variables_order\]' "$pool"; then
  printf '\nphp_admin_value[variables_order] = "EGPCS"\n' | sudo tee -a "$pool" >/dev/null
fi
sudo sed -i '/^env\[A8C_/d' "$pool"
{
  echo "env[A8C_SECRET_ONE] = secret-one-${version}"
  echo "env[A8C_SECRET_TWO] = secret-two-${version}"
  echo "env[A8C_KEEP_ME] = keep-me-${version}"
} | sudo tee -a "$pool" >/dev/null

sudo tee "/etc/nginx/sites-available/a8c-sapi-putenv-${version}" >/dev/null <<NGINX
server {
    listen 127.0.0.1:${port};
    server_name _;
    root /var/www/a8c-sapi-putenv;
    index probe.php;

    location ~ \.php$ {
        include snippets/fastcgi-php.conf;
        fastcgi_param A8C_SECRET_ONE "secret-one-${version}";
        fastcgi_param A8C_SECRET_TWO "secret-two-${version}";
        fastcgi_param A8C_KEEP_ME "keep-me-${version}";
        fastcgi_pass unix:/run/php/php${version}-fpm.sock;
    }
}
NGINX
sudo ln -sf "/etc/nginx/sites-available/a8c-sapi-putenv-${version}" "/etc/nginx/sites-enabled/a8c-sapi-putenv-${version}"
sudo "php-fpm${version}" -t
sudo systemctl restart "php${version}-fpm"
sudo nginx -t
sudo systemctl restart nginx

probe="$(curl -fsS "http://127.0.0.1:${port}/probe.php")"
scrub="$(curl -fsS "http://127.0.0.1:${port}/scrub.php")"

printf '%s\n' "$probe" > "$remote_dir/probe-${version}.json"
printf '%s\n' "$scrub" > "$remote_dir/scrub-${version}.json"

jq -e --arg version "$version" '
  .sapi == "fpm-fcgi"
  and .extension_loaded == true
  and .keys.A8C_SECRET_ONE.env_superglobal == ("secret-one-" + $version)
  and .keys.A8C_SECRET_ONE.server_superglobal == ("secret-one-" + $version)
  and .keys.A8C_SECRET_ONE.getenv_local == ("secret-one-" + $version)
  and .keys.A8C_SECRET_ONE.getenv_sapi == ("secret-one-" + $version)
  and .keys.A8C_SECRET_TWO.getenv_sapi == ("secret-two-" + $version)
  and .keys.A8C_KEEP_ME.getenv_sapi == ("keep-me-" + $version)
' "$remote_dir/probe-${version}.json" >/dev/null

jq -e --arg version "$version" '
  .sapi == "fpm-fcgi"
  and .extension_loaded == true
  and .after.A8C_SECRET_ONE.env_superglobal == false
  and .after.A8C_SECRET_ONE.server_superglobal == false
  and .after.A8C_SECRET_ONE.getenv_local == false
  and .after.A8C_SECRET_ONE.getenv_sapi == false
  and .after.A8C_SECRET_TWO.getenv_sapi == false
  and .after.A8C_KEEP_ME.env_superglobal == ("keep-me-" + $version)
  and .after.A8C_KEEP_ME.server_superglobal == ("keep-me-" + $version)
  and .after.A8C_KEEP_ME.getenv_local == ("keep-me-" + $version)
  and .after.A8C_KEEP_ME.getenv_sapi == ("keep-me-" + $version)
' "$remote_dir/scrub-${version}.json" >/dev/null

echo "PHP ${version}: baseline and scrub verification passed on http://127.0.0.1:${port}"
REMOTE
done

log "Copying verification artifacts back to docs/verification"
mkdir -p "$repo_root/docs/verification"
for version in "${VERSIONS[@]}"; do
  limactl copy "$LIMA_INSTANCE:$REMOTE_DIR/probe-${version}.json" "$repo_root/docs/verification/"
  limactl copy "$LIMA_INSTANCE:$REMOTE_DIR/scrub-${version}.json" "$repo_root/docs/verification/"
done

log "All requested PHP versions passed"
