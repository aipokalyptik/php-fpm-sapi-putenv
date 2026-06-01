PHP_ARG_ENABLE([a8c_sapi_putenv],
  [whether to enable a8c_sapi_putenv support],
  [AS_HELP_STRING([--enable-a8c-sapi-putenv],
    [Enable the a8c_sapi_putenv extension])],
  [no])

if test "$PHP_A8C_SAPI_PUTENV" != "no"; then
  PHP_NEW_EXTENSION([a8c_sapi_putenv],
    [a8c_sapi_putenv.c src/a8c_sapi_env.c],
    [$ext_shared])
fi
