# php-fpm SAPI putenv

This repository contains two implementations for scrubbing php-fpm FastCGI
request parameters from PHP environment read paths:

- `zend-extension/`: a standalone Zend extension for stock PHP builds.
- `php-patches/`: small PHP source patches that add the same capability as a
  built-in function guarded by a system INI setting.

The Zend extension remains useful for unpatched distro PHP packages. The PHP
patches are the preferred approach when the PHP build can be controlled because
they avoid loading a separate extension.
