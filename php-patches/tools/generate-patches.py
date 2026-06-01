#!/usr/bin/env python3
"""Generate versioned PHP source patches for a8c_sapi_putenv()."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
from typing import Iterable


VERSIONS = ("8.0", "8.1", "8.2", "8.3", "8.4", "8.5")


def replace_once(text: str, old: str, new: str, path: pathlib.Path) -> str:
    if old not in text:
        raise RuntimeError(f"pattern not found in {path}: {old[:80]!r}")
    return text.replace(old, new, 1)


def run(cmd: Iterable[str], cwd: pathlib.Path) -> None:
    subprocess.run(list(cmd), cwd=cwd, check=True)


def patch_sapi_h(path: pathlib.Path) -> None:
    text = path.read_text()
    text = replace_once(
        text,
        "SAPI_API char *sapi_getenv(const char *name, size_t name_len);\n",
        "SAPI_API char *sapi_getenv(const char *name, size_t name_len);\n"
        "SAPI_API zend_result sapi_putenv(const char *name, size_t name_len, const char *value);\n",
        path,
    )

    marker = "#define STANDARD_SAPI_MODULE_PROPERTIES \\\n"
    if "STANDARD_SAPI_MODULE_PROPERTIES_EX(sapi_putenv)" not in text:
        text = text.replace(marker, "#define STANDARD_SAPI_MODULE_PROPERTIES STANDARD_SAPI_MODULE_PROPERTIES_EX(NULL)\n\n#define STANDARD_SAPI_MODULE_PROPERTIES_EX(sapi_putenv) \\\n", 1)

    if "NULL  /* pre_request_init" in text:
        text = replace_once(
            text,
            "\tNULL  /* pre_request_init        */\n",
            "\tNULL, /* pre_request_init        */ \\\n"
            "\tsapi_putenv /* putenv          */\n",
            path,
        )
    else:
        text = replace_once(
            text,
            "\tNULL  /* input_filter_init       */\n",
            "\tNULL, /* input_filter_init       */ \\\n"
            "\tsapi_putenv /* putenv          */\n",
            path,
        )

    if "zend_result (*putenv)(const char *name, size_t name_len, const char *value);" not in text:
        if "\tint (*pre_request_init)(void);" in text:
            text = replace_once(
                text,
                "\tint (*pre_request_init)(void); /* called before activate and before the post data read - used for .user.ini */\n",
                "\tint (*pre_request_init)(void); /* called before activate and before the post data read - used for .user.ini */\n"
                "\tzend_result (*putenv)(const char *name, size_t name_len, const char *value);\n",
                path,
            )
        else:
            text = replace_once(
                text,
                "\tunsigned int (*input_filter_init)(void);\n",
                "\tunsigned int (*input_filter_init)(void);\n"
                "\tzend_result (*putenv)(const char *name, size_t name_len, const char *value);\n",
                path,
            )

    path.write_text(text)


def patch_sapi_c(path: pathlib.Path) -> None:
    text = path.read_text()
    if "SAPI_API zend_result sapi_putenv" not in text:
        start = text.index("SAPI_API char *sapi_getenv(const char *name, size_t name_len)")
        end = text.index("\n}\n\n", start) + len("\n}\n")
        addition = """
SAPI_API zend_result sapi_putenv(const char *name, size_t name_len, const char *value)
{
	if (sapi_module.putenv) {
		return sapi_module.putenv(name, name_len, value);
	}
	return FAILURE;
}
"""
        text = text[:end] + addition + text[end:]
    path.write_text(text)


def patch_fpm(path: pathlib.Path) -> None:
    text = path.read_text()
    disabled = """#if 0
static char *_sapi_cgibin_putenv(char *name, char *value) /* {{{ */
{
	int name_len;

	if (!name) {
		return NULL;
	}
	name_len = strlen(name);

	fcgi_request *request = (fcgi_request*) SG(server_context);
	return fcgi_putenv(request, name, name_len, value);
}
/* }}} */
#endif
"""
    enabled = """static zend_result sapi_cgibin_putenv(const char *name, size_t name_len, const char *value) /* {{{ */
{
	fcgi_request *request = (fcgi_request*) SG(server_context);

	if (!fpm_is_running || !fcgi_has_env(request) || name_len > INT_MAX) {
		return FAILURE;
	}

	fcgi_putenv(request, (char *) name, (int) name_len, (char *) value);
	return SUCCESS;
}
/* }}} */
"""
    text = replace_once(text, disabled, enabled, path)
    text = replace_once(
        text,
        "\tSTANDARD_SAPI_MODULE_PROPERTIES\n",
        "\tSTANDARD_SAPI_MODULE_PROPERTIES_EX(sapi_cgibin_putenv)\n",
        path,
    )
    path.write_text(text)


def patch_basic_header(path: pathlib.Path, version: str) -> None:
    text = path.read_text()
    bool_type = "zend_bool" if version in {"8.0", "8.1"} else "bool"
    if "a8c_sapi_putenv_enabled" not in text:
        text = replace_once(
            text,
            "\tHashTable putenv_ht;\n",
            "\tHashTable putenv_ht;\n"
            f"\t{bool_type} a8c_sapi_putenv_enabled;\n",
            path,
        )
    path.write_text(text)


def helper_80() -> str:
    return r'''#if defined(HAVE_PUTENV)
static zend_result php_putenv_impl(const char *setting, size_t setting_len, bool update_sapi) /* {{{ */
{
	char *p, **env;
	putenv_entry pe;
#ifdef PHP_WIN32
	char *value = NULL;
	int equals = 0;
	int error_code;
#endif

	if (setting_len == 0 || setting[0] == '=') {
		zend_argument_value_error(1, "must have a valid syntax");
		return FAILURE;
	}

	if (update_sapi && sapi_module.putenv == NULL) {
		return FAILURE;
	}

	pe.putenv_string = estrndup(setting, setting_len);
	pe.key = estrndup(setting, setting_len);
	if ((p = strchr(pe.key, '='))) {	/* nullify the '=' if there is one */
		*p = '\0';
#ifdef PHP_WIN32
		equals = 1;
#endif
	}

	pe.key_len = strlen(pe.key);
#ifdef PHP_WIN32
	if (equals) {
		if (pe.key_len < setting_len - 1) {
			value = p + 1;
		} else {
			/* empty string*/
			value = p;
		}
	}
#endif

	tsrm_env_lock();
	zend_hash_str_del(&BG(putenv_ht), pe.key, pe.key_len);

	/* find previous value */
	pe.previous_value = NULL;
	for (env = environ; env != NULL && *env != NULL; env++) {
		if (!strncmp(*env, pe.key, pe.key_len) && (*env)[pe.key_len] == '=') {	/* found it */
#if defined(PHP_WIN32)
			/* must copy previous value because MSVCRT's putenv can free the string without notice */
			pe.previous_value = estrdup(*env);
#else
			pe.previous_value = *env;
#endif
			break;
		}
	}

#if HAVE_UNSETENV
	if (!p) { /* no '=' means we want to unset it */
		unsetenv(pe.putenv_string);
	}
	if (!p || putenv(pe.putenv_string) == 0) { /* success */
#else
# ifndef PHP_WIN32
	if (putenv(pe.putenv_string) == 0) { /* success */
# else
		wchar_t *keyw, *valw = NULL;

		keyw = php_win32_cp_any_to_w(pe.key);
		if (value) {
			valw = php_win32_cp_any_to_w(value);
		}
		/* valw may be NULL, but the failed conversion still needs to be checked. */
		if (!keyw || !valw && value) {
			tsrm_env_unlock();
			efree(pe.putenv_string);
			efree(pe.key);
			free(keyw);
			free(valw);
			return FAILURE;
		}

	error_code = SetEnvironmentVariableW(keyw, valw);

	if (error_code != 0
# ifndef ZTS
	/* We need both SetEnvironmentVariable and _putenv here as some
		dependency lib could use either way to read the environment.
		Obviously the CRT version will be useful more often. But
		generally, doing both brings us on the safe track at least
		in NTS build. */
	&& _wputenv_s(keyw, valw ? valw : L"") == 0
# endif
	) { /* success */
# endif
#endif
		if (update_sapi && sapi_putenv(pe.key, pe.key_len, p ? p + 1 : NULL) != SUCCESS) {
			tsrm_env_unlock();
#if defined(PHP_WIN32)
			free(keyw);
			free(valw);
#endif
			efree(pe.putenv_string);
			efree(pe.key);
			return FAILURE;
		}
		zend_hash_str_add_mem(&BG(putenv_ht), pe.key, pe.key_len, &pe, sizeof(putenv_entry));
#ifdef HAVE_TZSET
		if (!strncmp(pe.key, "TZ", pe.key_len)) {
			tzset();
		}
#endif
		tsrm_env_unlock();
#if defined(PHP_WIN32)
		free(keyw);
		free(valw);
#endif
		return SUCCESS;
	} else {
		tsrm_env_unlock();
		efree(pe.putenv_string);
		efree(pe.key);
#if defined(PHP_WIN32)
		free(keyw);
		free(valw);
#endif
		return FAILURE;
	}
}
/* }}} */

/* {{{ Set the value of an environment variable */
PHP_FUNCTION(putenv)
{
	char *setting;
	size_t setting_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(setting, setting_len)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(php_putenv_impl(setting, setting_len, false) == SUCCESS);
}
/* }}} */

/* {{{ Set the value of an environment variable and the current SAPI request environment */
PHP_FUNCTION(a8c_sapi_putenv)
{
	char *setting;
	size_t setting_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(setting, setting_len)
	ZEND_PARSE_PARAMETERS_END();

	if (!BG(a8c_sapi_putenv_enabled)) {
		zend_throw_error(NULL, "a8c_sapi_putenv() is disabled");
		RETURN_THROWS();
	}

	RETURN_BOOL(php_putenv_impl(setting, setting_len, true) == SUCCESS);
}
/* }}} */
#endif
'''


def helper_81_plus() -> str:
    return r'''#ifdef HAVE_PUTENV
static zend_result php_putenv_impl(const char *setting, size_t setting_len, bool update_sapi) /* {{{ */
{
	char *p, **env;
	putenv_entry pe;
#ifdef PHP_WIN32
	const char *value = NULL;
	int error_code;
#endif

	if (setting_len == 0 || setting[0] == '=') {
		zend_argument_value_error(1, "must have a valid syntax");
		return FAILURE;
	}

	if (update_sapi && sapi_module.putenv == NULL) {
		return FAILURE;
	}

	pe.putenv_string = zend_strndup(setting, setting_len);
	if ((p = strchr(setting, '='))) {
		pe.key = zend_string_init(setting, p - setting, 0);
#ifdef PHP_WIN32
		value = p + 1;
#endif
	} else {
		pe.key = zend_string_init(setting, setting_len, 0);
	}

	tsrm_env_lock();
	zend_hash_del(&BG(putenv_ht), pe.key);

	/* find previous value */
	pe.previous_value = NULL;
	for (env = environ; env != NULL && *env != NULL; env++) {
		if (!strncmp(*env, ZSTR_VAL(pe.key), ZSTR_LEN(pe.key))
				&& (*env)[ZSTR_LEN(pe.key)] == '=') {	/* found it */
#ifdef PHP_WIN32
			/* must copy previous value because MSVCRT's putenv can free the string without notice */
			pe.previous_value = estrdup(*env);
#else
			pe.previous_value = *env;
#endif
			break;
		}
	}

#ifdef HAVE_UNSETENV
	if (!p) { /* no '=' means we want to unset it */
		unsetenv(pe.putenv_string);
	}
	if (!p || putenv(pe.putenv_string) == 0) { /* success */
#else
# ifndef PHP_WIN32
	if (putenv(pe.putenv_string) == 0) { /* success */
# else
		wchar_t *keyw, *valw = NULL;

		keyw = php_win32_cp_any_to_w(ZSTR_VAL(pe.key));
		if (value) {
			valw = php_win32_cp_any_to_w(value);
		}
		/* valw may be NULL, but the failed conversion still needs to be checked. */
		if (!keyw || (!valw && value)) {
			tsrm_env_unlock();
			free(pe.putenv_string);
			zend_string_release(pe.key);
			free(keyw);
			free(valw);
			return FAILURE;
		}

	error_code = SetEnvironmentVariableW(keyw, valw);

	if (error_code != 0
# ifndef ZTS
	/* We need both SetEnvironmentVariable and _putenv here as some
		dependency lib could use either way to read the environment.
		Obviously the CRT version will be useful more often. But
		generally, doing both brings us on the safe track at least
		in NTS build. */
	&& _wputenv_s(keyw, valw ? valw : L"") == 0
# endif
	) { /* success */
# endif
#endif
		if (update_sapi && sapi_putenv(ZSTR_VAL(pe.key), ZSTR_LEN(pe.key), p ? p + 1 : NULL) != SUCCESS) {
			tsrm_env_unlock();
#ifdef PHP_WIN32
			free(keyw);
			free(valw);
#endif
			free(pe.putenv_string);
			zend_string_release(pe.key);
			return FAILURE;
		}
		zend_hash_add_mem(&BG(putenv_ht), pe.key, &pe, sizeof(putenv_entry));
#ifdef HAVE_TZSET
		if (zend_string_equals_literal_ci(pe.key, "TZ")) {
			tzset();
		}
#endif
		tsrm_env_unlock();
#ifdef PHP_WIN32
		free(keyw);
		free(valw);
#endif
		return SUCCESS;
	} else {
		tsrm_env_unlock();
		free(pe.putenv_string);
		zend_string_release(pe.key);
#ifdef PHP_WIN32
		free(keyw);
		free(valw);
#endif
		return FAILURE;
	}
}
/* }}} */

/* {{{ Set the value of an environment variable */
PHP_FUNCTION(putenv)
{
	char *setting;
	size_t setting_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(setting, setting_len)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(php_putenv_impl(setting, setting_len, false) == SUCCESS);
}
/* }}} */

/* {{{ Set the value of an environment variable and the current SAPI request environment */
PHP_FUNCTION(a8c_sapi_putenv)
{
	char *setting;
	size_t setting_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(setting, setting_len)
	ZEND_PARSE_PARAMETERS_END();

	if (!BG(a8c_sapi_putenv_enabled)) {
		zend_throw_error(NULL, "a8c_sapi_putenv() is disabled");
		RETURN_THROWS();
	}

	RETURN_BOOL(php_putenv_impl(setting, setting_len, true) == SUCCESS);
}
/* }}} */
#endif
'''


def patch_basic_c(path: pathlib.Path, version: str) -> None:
    text = path.read_text()
    if "a8c_sapi_putenv.enable" not in text:
        text = replace_once(
            text,
            "zend_module_entry basic_functions_module = { /* {{{ */\n",
            "PHP_INI_BEGIN()\n"
            "\tSTD_PHP_INI_BOOLEAN(\"a8c_sapi_putenv.enable\", \"0\", PHP_INI_SYSTEM, OnUpdateBool, a8c_sapi_putenv_enabled, php_basic_globals, basic_globals)\n"
            "PHP_INI_END()\n\n"
            "zend_module_entry basic_functions_module = { /* {{{ */\n",
            path,
        )
    if "REGISTER_INI_ENTRIES();" not in text:
        if version == "8.0":
            text = replace_once(text, "\n\tphp_register_incomplete_class();\n", "\n\tREGISTER_INI_ENTRIES();\n\n\tphp_register_incomplete_class();\n", path)
        elif version == "8.1":
            text = replace_once(text, "\n\tphp_ce_incomplete_class = register_class___PHP_Incomplete_Class();\n", "\n\tREGISTER_INI_ENTRIES();\n\n\tphp_ce_incomplete_class = register_class___PHP_Incomplete_Class();\n", path)
        else:
            text = replace_once(text, "\n\tregister_basic_functions_symbols(module_number);\n", "\n\tREGISTER_INI_ENTRIES();\n\n\tregister_basic_functions_symbols(module_number);\n", path)
    if "UNREGISTER_INI_ENTRIES();" not in text:
        text = replace_once(text, "\nPHP_MSHUTDOWN_FUNCTION(basic) /* {{{ */\n{\n", "\nPHP_MSHUTDOWN_FUNCTION(basic) /* {{{ */\n{\n\tUNREGISTER_INI_ENTRIES();\n", path)

    old = extract_putenv_function(text)
    text = text.replace(old, helper_80() if version == "8.0" else helper_81_plus(), 1)
    path.write_text(text)


def extract_putenv_function(text: str) -> str:
    func_start = text.index("PHP_FUNCTION(putenv)")
    if_start_defined = text.rfind("#if defined(HAVE_PUTENV)", 0, func_start)
    if_start_ifdef = text.rfind("#ifdef HAVE_PUTENV", 0, func_start)
    start = max(if_start_defined, if_start_ifdef)
    if start < 0:
        raise RuntimeError("could not find HAVE_PUTENV guard for putenv")

    brace = text.index("{", func_start)
    depth = 0
    close = None
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                close = i + 1
                break
    if close is None:
        raise RuntimeError("could not find end of PHP_FUNCTION(putenv)")

    end = text.index("\n#endif", close) + len("\n#endif\n")
    return text[start:end]


def patch_stub(path: pathlib.Path) -> None:
    text = path.read_text()
    if "function a8c_sapi_putenv" not in text:
        text = replace_once(
            text,
            "#ifdef HAVE_PUTENV\nfunction putenv(string $assignment): bool {}\n#endif\n",
            "#ifdef HAVE_PUTENV\nfunction putenv(string $assignment): bool {}\n\nfunction a8c_sapi_putenv(string $assignment): bool {}\n#endif\n",
            path,
        )
    path.write_text(text)


def patch_arginfo(path: pathlib.Path) -> None:
    text = path.read_text()
    if "arginfo_a8c_sapi_putenv" not in text:
        text = replace_once(
            text,
            "ZEND_END_ARG_INFO()\n#endif\n",
            "ZEND_END_ARG_INFO()\n\n"
            "#define arginfo_a8c_sapi_putenv arginfo_putenv\n"
            "#endif\n",
            path,
        )
    if "ZEND_FE(a8c_sapi_putenv" not in text:
        text = replace_once(
            text,
            "ZEND_FUNCTION(putenv);\n#endif\n",
            "ZEND_FUNCTION(putenv);\n"
            "ZEND_FUNCTION(a8c_sapi_putenv);\n"
            "#endif\n",
            path,
        )
        text = replace_once(
            text,
            "\tZEND_FE(putenv, arginfo_putenv)\n#endif\n",
            "\tZEND_FE(putenv, arginfo_putenv)\n"
            "\tZEND_FE(a8c_sapi_putenv, arginfo_a8c_sapi_putenv)\n"
            "#endif\n",
            path,
        )
    path.write_text(text)


def patch_version(src: pathlib.Path, version: str) -> None:
    patch_sapi_h(src / "main" / "SAPI.h")
    patch_sapi_c(src / "main" / "SAPI.c")
    patch_fpm(src / "sapi" / "fpm" / "fpm" / "fpm_main.c")
    patch_basic_header(src / "ext" / "standard" / "basic_functions.h", version)
    patch_basic_c(src / "ext" / "standard" / "basic_functions.c", version)
    patch_stub(src / "ext" / "standard" / "basic_functions.stub.php")
    patch_arginfo(src / "ext" / "standard" / "basic_functions_arginfo.h")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src-root", default="/tmp/a8c_php_patch_refs")
    parser.add_argument("--out-dir", default="php-patches/patches")
    args = parser.parse_args()

    src_root = pathlib.Path(args.src_root)
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    for version in VERSIONS:
        src = src_root / f"php-{version}"
        run(["git", "reset", "--hard", f"PHP-{version}.0"], src)
        patch_version(src, version)
        diff = subprocess.run(["git", "diff", "--text"], cwd=src, check=True, text=True, stdout=subprocess.PIPE).stdout
        (out_dir / f"php-{version}.patch").write_text(diff)


if __name__ == "__main__":
    main()
