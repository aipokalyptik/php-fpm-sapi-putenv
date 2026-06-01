#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "SAPI.h"
#include "ext/standard/basic_functions.h"

#include "a8c_sapi_env.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(PHP_WIN32)
# include <netinet/tcp.h>
extern char **environ;
#endif

#define A8C_FCGI_HASH_TABLE_SIZE 128
#define A8C_FCGI_HASH_SEG_SIZE 4096

typedef struct _a8c_putenv_entry {
	char *putenv_string;
	char *previous_value;
#if PHP_VERSION_ID < 80100
	char *key;
	size_t key_len;
#else
	zend_string *key;
#endif
} a8c_putenv_entry;

typedef struct _a8c_fcgi_hash_bucket {
	unsigned int reserved_hash_value;
	unsigned int var_len;
	char *var;
	unsigned int val_len;
	char *val;
	struct _a8c_fcgi_hash_bucket *reserved_chain_next;
	struct _a8c_fcgi_hash_bucket *list_next;
} a8c_fcgi_hash_bucket;

typedef struct _a8c_fcgi_hash_buckets {
	unsigned int reserved_idx;
	struct _a8c_fcgi_hash_buckets *reserved_next;
	a8c_fcgi_hash_bucket reserved_data[A8C_FCGI_HASH_TABLE_SIZE];
} a8c_fcgi_hash_buckets;

typedef struct _a8c_fcgi_data_seg {
	char *pos;
	char *end;
	struct _a8c_fcgi_data_seg *next;
	char data[1];
} a8c_fcgi_data_seg;

typedef struct _a8c_fcgi_hash {
	a8c_fcgi_hash_bucket *reserved_hash_table[A8C_FCGI_HASH_TABLE_SIZE];
	a8c_fcgi_hash_bucket *list;
	a8c_fcgi_hash_buckets *reserved_buckets;
	a8c_fcgi_data_seg *data;
} a8c_fcgi_hash;

typedef struct _a8c_fcgi_end_request {
	unsigned char reserved_app_status_b3;
	unsigned char reserved_app_status_b2;
	unsigned char reserved_app_status_b1;
	unsigned char reserved_app_status_b0;
	unsigned char reserved_protocol_status;
	unsigned char reserved[3];
} a8c_fcgi_end_request;

typedef struct _a8c_fcgi_header {
	unsigned char reserved_version;
	unsigned char reserved_type;
	unsigned char reserved_request_id_b1;
	unsigned char reserved_request_id_b0;
	unsigned char reserved_content_length_b1;
	unsigned char reserved_content_length_b0;
	unsigned char reserved_padding_length;
	unsigned char reserved;
} a8c_fcgi_header;

typedef struct _a8c_fcgi_end_request_rec {
	a8c_fcgi_header hdr;
	a8c_fcgi_end_request body;
} a8c_fcgi_end_request_rec;

/*
 * php-fpm keeps FastCGI params in this private request structure. The layout has
 * been stable across PHP 8.0-8.5; keep this copy local so the scrub helper can
 * be dropped into another extension without depending on non-installed headers.
 */
typedef struct _a8c_fcgi_request {
	int reserved_listen_socket;
	int reserved_tcp;
	int reserved_fd;
	int reserved_id;
	int reserved_keep;
#ifdef TCP_NODELAY
	int reserved_nodelay;
#endif
	int reserved_ended;
	int reserved_in_len;
	int reserved_in_pad;
	a8c_fcgi_header *reserved_out_hdr;
	unsigned char *reserved_out_pos;
	unsigned char reserved_out_buf[1024 * 8];
	unsigned char reserved[sizeof(a8c_fcgi_end_request_rec)];
	void (*reserved_hook_on_accept)(void);
	void (*reserved_hook_on_read)(void);
	void (*reserved_hook_on_close)(void);
	int has_env;
	a8c_fcgi_hash env;
} a8c_fcgi_request;

static void a8c_putenv_entry_init(a8c_putenv_entry *entry, const char *setting, size_t setting_len, size_t key_len)
{
	entry->previous_value = NULL;

#if PHP_VERSION_ID < 80100
	entry->putenv_string = estrndup(setting, setting_len);
	entry->key = estrndup(setting, key_len);
	entry->key_len = key_len;
#else
	entry->putenv_string = zend_strndup(setting, setting_len);
	entry->key = zend_string_init(setting, key_len, 0);
#endif
}

static void a8c_putenv_entry_free(a8c_putenv_entry *entry)
{
#if PHP_VERSION_ID < 80100
	efree(entry->putenv_string);
	efree(entry->key);
#else
	free(entry->putenv_string);
	zend_string_release(entry->key);
#endif
}

static void a8c_putenv_entry_restore_previous(a8c_putenv_entry *entry)
{
	if (entry->previous_value != NULL) {
		putenv(entry->previous_value);
	} else {
#ifdef HAVE_UNSETENV
# if PHP_VERSION_ID < 80100
		unsetenv(entry->key);
# else
		unsetenv(ZSTR_VAL(entry->key));
# endif
#endif
	}
}

static zend_bool a8c_sapi_is_fastcgi_sapi(void)
{
	return sapi_module.name != NULL &&
		(strcmp(sapi_module.name, "fpm-fcgi") == 0 || strcmp(sapi_module.name, "cgi-fcgi") == 0);
}

static char *a8c_fcgi_hash_strndup(a8c_fcgi_hash *hash, const char *str, size_t str_len)
{
	char *ret;

	if (hash->data->pos + str_len + 1 >= hash->data->end) {
		size_t seg_size = (str_len + 1 > A8C_FCGI_HASH_SEG_SIZE) ? str_len + 1 : A8C_FCGI_HASH_SEG_SIZE;
		a8c_fcgi_data_seg *segment = malloc(sizeof(a8c_fcgi_data_seg) - 1 + seg_size);

		if (segment == NULL) {
			return NULL;
		}

		segment->pos = segment->data;
		segment->end = segment->pos + seg_size;
		segment->next = hash->data;
		hash->data = segment;
	}

	ret = hash->data->pos;
	memcpy(ret, str, str_len);
	ret[str_len] = '\0';
	hash->data->pos += str_len + 1;
	return ret;
}

static zend_result a8c_fcgi_hash_update_existing(a8c_fcgi_hash *hash, const char *var, size_t var_len, const char *val)
{
	a8c_fcgi_hash_bucket *bucket = hash->list;
	zend_result result = FAILURE;

	while (bucket != NULL) {
		if (bucket->var_len == var_len &&
				memcmp(bucket->var, var, var_len) == 0) {
			if (val == NULL) {
				bucket->val = NULL;
				bucket->val_len = 0;
				result = SUCCESS;
			} else {
				size_t val_len = strlen(val);
				if (val_len > (size_t) UINT_MAX) {
					return FAILURE;
				}
				bucket->val = a8c_fcgi_hash_strndup(hash, val, val_len);
				if (bucket->val == NULL) {
					return FAILURE;
				}
				bucket->val_len = (unsigned int) val_len;
				return SUCCESS;
			}
		}
		bucket = bucket->list_next;
	}

	return result;
}

static zend_result a8c_fcgi_request_putenv(const char *setting, size_t key_len, const char *value)
{
	a8c_fcgi_request *request;

	if (!a8c_sapi_is_fastcgi_sapi() || SG(server_context) == NULL) {
		return SUCCESS;
	}

	if (key_len > (size_t) INT_MAX || !((a8c_fcgi_request *) SG(server_context))->has_env) {
		return FAILURE;
	}

	request = (a8c_fcgi_request *) SG(server_context);
	return a8c_fcgi_hash_update_existing(&request->env, setting, key_len, value);
}

static zend_result a8c_sapi_split_setting(const char *setting, size_t setting_len, const char **value)
{
	const char *equals;

	if (setting_len == 0 || setting[0] == '=') {
		zend_argument_value_error(1, "must have a valid syntax");
		return FAILURE;
	}

	equals = memchr(setting, '=', setting_len);
	if (equals == NULL) {
		*value = NULL;
		return SUCCESS;
	}

	*value = equals + 1;
	return SUCCESS;
}

static zend_result a8c_sapi_update_fastcgi_request(const char *setting, size_t setting_len, const char *value)
{
	size_t key_len;

	if (!a8c_sapi_is_fastcgi_sapi() || SG(server_context) == NULL) {
		return SUCCESS;
	}

	key_len = value == NULL ? setting_len : (size_t) (value - setting - 1);
	return a8c_fcgi_request_putenv(setting, key_len, value);
}

static zend_result a8c_sapi_update_process_environment(const char *setting, size_t setting_len, const char *value)
{
	a8c_putenv_entry entry;
	char **env;
	size_t key_len = value == NULL ? setting_len : (size_t) (value - setting - 1);

#ifdef PHP_WIN32
	return FAILURE;
#else
	a8c_putenv_entry_init(&entry, setting, setting_len, key_len);

	tsrm_env_lock();
# if PHP_VERSION_ID < 80100
	zend_hash_str_del(&BG(putenv_ht), entry.key, entry.key_len);
# else
	zend_hash_del(&BG(putenv_ht), entry.key);
# endif

	for (env = environ; env != NULL && *env != NULL; env++) {
# if PHP_VERSION_ID < 80100
		if (!strncmp(*env, entry.key, entry.key_len) && (*env)[entry.key_len] == '=') {
# else
		if (!strncmp(*env, ZSTR_VAL(entry.key), ZSTR_LEN(entry.key))
				&& (*env)[ZSTR_LEN(entry.key)] == '=') {
# endif
			entry.previous_value = *env;
			break;
		}
	}

# ifdef HAVE_UNSETENV
	if (value == NULL) {
		unsetenv(entry.putenv_string);
	}
	if (value == NULL || putenv(entry.putenv_string) == 0) {
# else
	if (putenv(entry.putenv_string) == 0) {
# endif
# if PHP_VERSION_ID < 80100
		if (zend_hash_str_add_mem(&BG(putenv_ht), entry.key, entry.key_len, &entry, sizeof(a8c_putenv_entry)) == NULL) {
			a8c_putenv_entry_restore_previous(&entry);
			tsrm_env_unlock();
			a8c_putenv_entry_free(&entry);
			return FAILURE;
		}
# else
		if (zend_hash_add_mem(&BG(putenv_ht), entry.key, &entry, sizeof(a8c_putenv_entry)) == NULL) {
			a8c_putenv_entry_restore_previous(&entry);
			tsrm_env_unlock();
			a8c_putenv_entry_free(&entry);
			return FAILURE;
		}
# endif
# ifdef HAVE_TZSET
#  if PHP_VERSION_ID < 80100
		if (!strncmp(entry.key, "TZ", entry.key_len)) {
#  else
		if (zend_string_equals_literal_ci(entry.key, "TZ")) {
#  endif
			tzset();
		}
# endif
		tsrm_env_unlock();
		return SUCCESS;
	}

	tsrm_env_unlock();
	a8c_putenv_entry_free(&entry);
	return FAILURE;
#endif
}

zend_result a8c_sapi_putenv_update(const char *setting, size_t setting_len)
{
	const char *value;

	if (a8c_sapi_split_setting(setting, setting_len, &value) == FAILURE) {
		return FAILURE;
	}

	if (a8c_sapi_update_process_environment(setting, setting_len, value) == FAILURE) {
		return FAILURE;
	}

	return a8c_sapi_update_fastcgi_request(setting, setting_len, value);
}
