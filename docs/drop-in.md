# Drop-In Instructions

The reusable implementation is intentionally isolated in:

- `src/a8c_sapi_env.h`
- `src/a8c_sapi_env.c`

To move the function into another extension:

1. Copy both files into the target extension.
2. Add `src/a8c_sapi_env.c` to that extension's source list in `config.m4` or `config.w32`.
3. Include the helper header from the file that registers PHP functions:

```c
#include "src/a8c_sapi_env.h"
```

4. Register a PHP function that parses one string argument and calls the helper:

```c
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
```

5. Add arginfo and a function-table entry using the target extension's normal style:

```c
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_a8c_sapi_putenv, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, assignment, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FE(a8c_sapi_putenv, arginfo_a8c_sapi_putenv)
```

The helper calls PHP's built-in `putenv()` through the engine function table to preserve PHP's request-local environment bookkeeping, then updates php-fpm's FastCGI request environment. It first uses a weak `fcgi_putenv` symbol when available; if packaged php-fpm does not export that symbol, it falls back to the private FastCGI request hash layout verified across PHP 8.0-8.5.
