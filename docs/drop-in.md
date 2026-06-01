# Drop-In Instructions

The reusable implementation is intentionally isolated in:

- `src/a8c_sapi_env.h`
- `src/a8c_sapi_env.c`

The helper also needs one per-request module global, plus RINIT/RSHUTDOWN hooks
that initialize and destroy that global. This is what lets it restore the
process environment at request shutdown without calling PHP's registered
`putenv()` function.

To move the function into another extension:

1. Copy both files into the target extension.
2. Add `src/a8c_sapi_env.c` to that extension's source list in `config.m4` or `config.w32`.
3. In `src/a8c_sapi_env.c`, replace this extension's module-header include with
   the target extension's module header:

```c
/* Replace this: */
#include "php_a8c_sapi_putenv.h"

/* With the target extension header that defines A8C_SAPI_PUTENV_G(): */
#include "php_target_ext.h"
```

4. Add a `HashTable putenv_ht` field to the target extension's module globals
   and expose a globals accessor macro. If the target extension does not already
   use module globals, copy this pattern and replace `target_ext` with the
   target extension's module name:

```c
ZEND_BEGIN_MODULE_GLOBALS(target_ext)
	HashTable putenv_ht;
ZEND_END_MODULE_GLOBALS(target_ext)

ZEND_EXTERN_MODULE_GLOBALS(target_ext)

#define A8C_SAPI_PUTENV_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(target_ext, v)
```

5. Declare the globals once in the target extension's main C file:

```c
ZEND_DECLARE_MODULE_GLOBALS(target_ext)
```

6. Include the helper header from the file that registers PHP functions:

```c
#include "src/a8c_sapi_env.h"
```

7. Call the helper lifecycle functions from the target extension's request
   lifecycle hooks:

```c
PHP_RINIT_FUNCTION(target_ext)
{
#if defined(ZTS) && defined(COMPILE_DL_TARGET_EXT)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	a8c_sapi_putenv_request_init();
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(target_ext)
{
	a8c_sapi_putenv_request_shutdown();
	return SUCCESS;
}
```

8. Register a PHP function that parses one string argument and calls the helper:

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

9. Add arginfo and a function-table entry using the target extension's normal style:

```c
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_a8c_sapi_putenv, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, assignment, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FE(a8c_sapi_putenv, arginfo_a8c_sapi_putenv)
```

10. If the target extension already uses a different globals accessor name,
   either add a compatibility macro named `A8C_SAPI_PUTENV_G` or adjust
   `src/a8c_sapi_env.c` to call the target extension's existing accessor.

The helper owns process-environment mutation and request-shutdown restore bookkeeping, so it still works when `putenv` is listed in `disable_functions`. It then updates php-fpm's FastCGI request environment. It first uses a weak `fcgi_putenv` symbol when available; if packaged php-fpm does not export that symbol, it falls back to the private FastCGI request hash layout verified across PHP 8.0-8.5.
