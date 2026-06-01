#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_a8c_sapi_putenv.h"
#include "src/a8c_sapi_env.h"

PHP_FUNCTION(a8c_sapi_putenv);

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_a8c_sapi_putenv, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, assignment, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry a8c_sapi_putenv_functions[] = {
	PHP_FE(a8c_sapi_putenv, arginfo_a8c_sapi_putenv)
	PHP_FE_END
};

zend_module_entry a8c_sapi_putenv_module_entry = {
	STANDARD_MODULE_HEADER,
	"a8c_sapi_putenv",
	a8c_sapi_putenv_functions,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	PHP_A8C_SAPI_PUTENV_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_A8C_SAPI_PUTENV
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(a8c_sapi_putenv)
#endif

PHP_FUNCTION(a8c_sapi_putenv)
{
	char *setting;
	size_t setting_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(setting, setting_len)
	ZEND_PARSE_PARAMETERS_END();

	if (a8c_sapi_putenv_update(setting, setting_len) == FAILURE) {
		if (EG(exception)) {
			RETURN_THROWS();
		}
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
