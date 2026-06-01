# Drop-In Instructions

The reusable implementation is intentionally isolated in:

- `src/a8c_sapi_env.h`
- `src/a8c_sapi_env.c`

The helper mutates the process environment directly, then records the change in
PHP standard's existing `BG(putenv_ht)` request table. That keeps request
shutdown behavior identical to core `putenv()` and avoids a parallel restore
table. It does not call PHP's registered `putenv()` function, so it still works
when `putenv` is listed in `disable_functions`.

To move the function into another extension:

1. Copy both files into the target extension.
2. Add `src/a8c_sapi_env.c` to that extension's source list in `config.m4` or `config.w32`.
3. Make sure the target extension can include PHP standard's globals header:

```c
#include "ext/standard/basic_functions.h"
```

This header is already included by `src/a8c_sapi_env.c`; it provides `BG(putenv_ht)`.

4. Include the helper header from the file that registers PHP functions:

```c
#include "src/a8c_sapi_env.h"
```

5. Register a PHP function that parses one string argument and calls the helper:

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

6. Add arginfo and a function-table entry using the target extension's normal style:

```c
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_a8c_sapi_putenv, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, assignment, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FE(a8c_sapi_putenv, arginfo_a8c_sapi_putenv)
```

No target-extension module globals, RINIT hook, or RSHUTDOWN hook are required
for this helper. PHP standard owns `BG(putenv_ht)` and destroys it during its
normal request shutdown.

The helper updates php-fpm's FastCGI request environment after the process
environment update succeeds. It first uses a weak `fcgi_putenv` symbol when
available; if packaged php-fpm does not export that symbol, it falls back to the
private FastCGI request hash layout verified across PHP 8.0-8.5.
