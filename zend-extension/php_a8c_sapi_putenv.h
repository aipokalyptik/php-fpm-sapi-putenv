#ifndef PHP_A8C_SAPI_PUTENV_H
#define PHP_A8C_SAPI_PUTENV_H

extern zend_module_entry a8c_sapi_putenv_module_entry;
#define phpext_a8c_sapi_putenv_ptr &a8c_sapi_putenv_module_entry

#define PHP_A8C_SAPI_PUTENV_VERSION "0.1.0"

#if defined(ZTS) && defined(COMPILE_DL_A8C_SAPI_PUTENV)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_A8C_SAPI_PUTENV_H */
