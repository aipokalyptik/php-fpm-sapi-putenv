#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "SAPI.h"

#include "php_a8c_sapi_putenv.h"
#include "a8c_sapi_env.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(PHP_WIN32)
# include <netinet/tcp.h>
extern char **environ;
#endif

#define A8C_FCGI_HASH_TABLE_SIZE 128
#define A8C_FCGI_HASH_TABLE_MASK (A8C_FCGI_HASH_TABLE_SIZE - 1)
#define A8C_FCGI_HASH_SEG_SIZE 4096
#define A8C_FCGI_HASH_FUNC(var, var_len) \
	((var_len) < 3 ? (unsigned int) (var_len) : \
		(((unsigned int) (var)[3]) << 2) + \
		(((unsigned int) (var)[(var_len) - 2]) << 4) + \
		(((unsigned int) (var)[(var_len) - 1]) << 2) + \
		(unsigned int) (var_len))

typedef struct _fcgi_request fcgi_request;

typedef struct _a8c_putenv_entry {
	char *putenv_string;
	char *previous_value;
	zend_string *key;
} a8c_putenv_entry;

typedef struct _a8c_fcgi_hash_bucket {
	unsigned int hash_value;
	unsigned int var_len;
	char *var;
	unsigned int val_len;
	char *val;
	struct _a8c_fcgi_hash_bucket *next;
	struct _a8c_fcgi_hash_bucket *list_next;
} a8c_fcgi_hash_bucket;

typedef struct _a8c_fcgi_hash_buckets {
	unsigned int idx;
	struct _a8c_fcgi_hash_buckets *next;
	a8c_fcgi_hash_bucket data[A8C_FCGI_HASH_TABLE_SIZE];
} a8c_fcgi_hash_buckets;

typedef struct _a8c_fcgi_data_seg {
	char *pos;
	char *end;
	struct _a8c_fcgi_data_seg *next;
	char data[1];
} a8c_fcgi_data_seg;

typedef struct _a8c_fcgi_hash {
	a8c_fcgi_hash_bucket *hash_table[A8C_FCGI_HASH_TABLE_SIZE];
	a8c_fcgi_hash_bucket *list;
	a8c_fcgi_hash_buckets *buckets;
	a8c_fcgi_data_seg *data;
} a8c_fcgi_hash;

typedef struct _a8c_fcgi_req_hook {
	void (*on_accept)(void);
	void (*on_read)(void);
	void (*on_close)(void);
} a8c_fcgi_req_hook;

typedef struct _a8c_fcgi_end_request {
	unsigned char appStatusB3;
	unsigned char appStatusB2;
	unsigned char appStatusB1;
	unsigned char appStatusB0;
	unsigned char protocolStatus;
	unsigned char reserved[3];
} a8c_fcgi_end_request;

typedef struct _a8c_fcgi_header {
	unsigned char version;
	unsigned char type;
	unsigned char requestIdB1;
	unsigned char requestIdB0;
	unsigned char contentLengthB1;
	unsigned char contentLengthB0;
	unsigned char paddingLength;
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
	int listen_socket;
	int tcp;
	int fd;
	int id;
	int keep;
#ifdef TCP_NODELAY
	int nodelay;
#endif
	int ended;
	int in_len;
	int in_pad;
	a8c_fcgi_header *out_hdr;
	unsigned char *out_pos;
	unsigned char out_buf[1024 * 8];
	unsigned char reserved[sizeof(a8c_fcgi_end_request_rec)];
	a8c_fcgi_req_hook hook;
	int has_env;
	a8c_fcgi_hash env;
} a8c_fcgi_request;

/*
 * fcgi_putenv() is provided by php-fpm, not by every SAPI binary that may load
 * this extension. A weak declaration keeps the extension loadable in CLI while
 * still allowing php-fpm to resolve and use the symbol when it is exported.
 */
#if defined(__GNUC__) || defined(__clang__)
extern char *fcgi_putenv(fcgi_request *req, char *var, int var_len, char *val) __attribute__((weak));
#else
extern char *fcgi_putenv(fcgi_request *req, char *var, int var_len, char *val);
#endif

static void a8c_putenv_entry_dtor(zval *zv)
{
	a8c_putenv_entry *entry = Z_PTR_P(zv);

#ifndef PHP_WIN32
	if (entry->previous_value != NULL) {
		putenv(entry->previous_value);
	} else {
# ifdef HAVE_UNSETENV
		unsetenv(ZSTR_VAL(entry->key));
# else
		char **env;

		for (env = environ; env != NULL && *env != NULL; env++) {
			if (!strncmp(*env, ZSTR_VAL(entry->key), ZSTR_LEN(entry->key))
					&& (*env)[ZSTR_LEN(entry->key)] == '=') {
				*env = "";
				break;
			}
		}
# endif
	}
#endif

#ifdef HAVE_TZSET
	if (zend_string_equals_literal_ci(entry->key, "TZ")) {
		tzset();
	}
#endif

	free(entry->putenv_string);
	zend_string_release(entry->key);
	efree(entry);
}

void a8c_sapi_putenv_request_init(void)
{
	zend_hash_init(&A8C_SAPI_PUTENV_G(putenv_ht), 8, NULL, a8c_putenv_entry_dtor, 0);
}

void a8c_sapi_putenv_request_shutdown(void)
{
	zend_hash_destroy(&A8C_SAPI_PUTENV_G(putenv_ht));
}

static zend_bool a8c_sapi_is_fastcgi_sapi(void)
{
	return sapi_module.name != NULL &&
		(strcmp(sapi_module.name, "fpm-fcgi") == 0 || strcmp(sapi_module.name, "cgi-fcgi") == 0);
}

static char *a8c_fcgi_hash_strndup(a8c_fcgi_hash *hash, const char *str, unsigned int str_len)
{
	char *ret;

	if (hash->data->pos + str_len + 1 >= hash->data->end) {
		unsigned int seg_size = (str_len + 1 > A8C_FCGI_HASH_SEG_SIZE) ? str_len + 1 : A8C_FCGI_HASH_SEG_SIZE;
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

static zend_result a8c_fcgi_hash_set(a8c_fcgi_hash *hash, const char *var, unsigned int var_len, const char *val)
{
	unsigned int hash_value = A8C_FCGI_HASH_FUNC(var, var_len);
	unsigned int val_len = (unsigned int) strlen(val);
	unsigned int idx = hash_value & A8C_FCGI_HASH_TABLE_MASK;
	a8c_fcgi_hash_bucket *bucket = hash->hash_table[idx];

	while (bucket != NULL) {
		if (bucket->hash_value == hash_value &&
				bucket->var_len == var_len &&
				memcmp(bucket->var, var, var_len) == 0) {
			bucket->val_len = val_len;
			bucket->val = a8c_fcgi_hash_strndup(hash, val, val_len);
			return bucket->val == NULL ? FAILURE : SUCCESS;
		}
		bucket = bucket->next;
	}

	if (hash->buckets->idx >= A8C_FCGI_HASH_TABLE_SIZE) {
		a8c_fcgi_hash_buckets *buckets = malloc(sizeof(a8c_fcgi_hash_buckets));
		if (buckets == NULL) {
			return FAILURE;
		}
		buckets->idx = 0;
		buckets->next = hash->buckets;
		hash->buckets = buckets;
	}

	bucket = hash->buckets->data + hash->buckets->idx;
	hash->buckets->idx++;
	bucket->next = hash->hash_table[idx];
	hash->hash_table[idx] = bucket;
	bucket->list_next = hash->list;
	hash->list = bucket;
	bucket->hash_value = hash_value;
	bucket->var_len = var_len;
	bucket->var = a8c_fcgi_hash_strndup(hash, var, var_len);
	bucket->val_len = val_len;
	bucket->val = a8c_fcgi_hash_strndup(hash, val, val_len);

	return bucket->var == NULL || bucket->val == NULL ? FAILURE : SUCCESS;
}

static void a8c_fcgi_hash_del(a8c_fcgi_hash *hash, const char *var, unsigned int var_len)
{
	unsigned int hash_value = A8C_FCGI_HASH_FUNC(var, var_len);
	unsigned int idx = hash_value & A8C_FCGI_HASH_TABLE_MASK;
	a8c_fcgi_hash_bucket **bucket = &hash->hash_table[idx];

	while (*bucket != NULL) {
		if ((*bucket)->hash_value == hash_value &&
				(*bucket)->var_len == var_len &&
				memcmp((*bucket)->var, var, var_len) == 0) {
			(*bucket)->val = NULL;
			(*bucket)->val_len = 0;
			*bucket = (*bucket)->next;
			return;
		}
		bucket = &(*bucket)->next;
	}
}

static zend_result a8c_fcgi_request_putenv(const char *setting, size_t key_len, const char *value)
{
	a8c_fcgi_request *request;

	if (!a8c_sapi_is_fastcgi_sapi() || SG(server_context) == NULL) {
		return SUCCESS;
	}

	if (key_len > (size_t) UINT_MAX || !((a8c_fcgi_request *) SG(server_context))->has_env) {
		return FAILURE;
	}

	if (fcgi_putenv != NULL) {
		fcgi_putenv((fcgi_request *) SG(server_context), (char *) setting, (int) key_len, (char *) value);
		return SUCCESS;
	}

	request = (a8c_fcgi_request *) SG(server_context);
	if (value == NULL) {
		a8c_fcgi_hash_del(&request->env, setting, (unsigned int) key_len);
		return SUCCESS;
	}

	return a8c_fcgi_hash_set(&request->env, setting, (unsigned int) key_len, value);
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
	entry.putenv_string = zend_strndup(setting, setting_len);
	entry.key = zend_string_init(setting, key_len, 0);
	entry.previous_value = NULL;

	tsrm_env_lock();
	zend_hash_del(&A8C_SAPI_PUTENV_G(putenv_ht), entry.key);

	for (env = environ; env != NULL && *env != NULL; env++) {
		if (!strncmp(*env, ZSTR_VAL(entry.key), ZSTR_LEN(entry.key))
				&& (*env)[ZSTR_LEN(entry.key)] == '=') {
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
		zend_hash_add_mem(&A8C_SAPI_PUTENV_G(putenv_ht), entry.key, &entry, sizeof(a8c_putenv_entry));
# ifdef HAVE_TZSET
		if (zend_string_equals_literal_ci(entry.key, "TZ")) {
			tzset();
		}
# endif
		tsrm_env_unlock();
		return SUCCESS;
	}

	tsrm_env_unlock();
	free(entry.putenv_string);
	zend_string_release(entry.key);
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
