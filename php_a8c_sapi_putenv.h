#ifndef PHP_A8C_SAPI_PUTENV_H
#define PHP_A8C_SAPI_PUTENV_H

extern zend_module_entry a8c_sapi_putenv_module_entry;
#define phpext_a8c_sapi_putenv_ptr &a8c_sapi_putenv_module_entry

#define PHP_A8C_SAPI_PUTENV_VERSION "0.1.0"

ZEND_BEGIN_MODULE_GLOBALS(a8c_sapi_putenv)
	HashTable putenv_ht;
ZEND_END_MODULE_GLOBALS(a8c_sapi_putenv)

ZEND_EXTERN_MODULE_GLOBALS(a8c_sapi_putenv)

#define A8C_SAPI_PUTENV_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(a8c_sapi_putenv, v)

#if defined(ZTS) && defined(COMPILE_DL_A8C_SAPI_PUTENV)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif /* PHP_A8C_SAPI_PUTENV_H */
