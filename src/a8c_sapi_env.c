#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "SAPI.h"

#include "a8c_sapi_env.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(PHP_WIN32)
# include <netinet/tcp.h>
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

static void a8c_sapi_update_fastcgi_request(const char *setting, size_t setting_len, const char *value)
{
	size_t key_len;

	if (!a8c_sapi_is_fastcgi_sapi() || SG(server_context) == NULL) {
		return;
	}

	key_len = value == NULL ? setting_len : (size_t) (value - setting - 1);
	a8c_fcgi_request_putenv(setting, key_len, value);
}

static zend_result a8c_sapi_call_putenv(const char *setting, size_t setting_len)
{
	zval function_name;
	zval argument;
	zval retval;
	zend_result result;

	ZVAL_STRINGL(&function_name, "putenv", sizeof("putenv") - 1);
	ZVAL_STRINGL(&argument, setting, setting_len);
	ZVAL_UNDEF(&retval);

	result = call_user_function(EG(function_table), NULL, &function_name, &retval, 1, &argument);

	zval_ptr_dtor(&function_name);
	zval_ptr_dtor(&argument);

	if (result == FAILURE || EG(exception)) {
		if (!Z_ISUNDEF(retval)) {
			zval_ptr_dtor(&retval);
		}
		return FAILURE;
	}

	if (Z_TYPE(retval) != IS_TRUE) {
		zval_ptr_dtor(&retval);
		return FAILURE;
	}

	zval_ptr_dtor(&retval);
	return SUCCESS;
}

zend_result a8c_sapi_putenv_update(const char *setting, size_t setting_len)
{
	const char *value;

	if (a8c_sapi_split_setting(setting, setting_len, &value) == FAILURE) {
		return FAILURE;
	}

	if (a8c_sapi_call_putenv(setting, setting_len) == FAILURE) {
		return FAILURE;
	}

	a8c_sapi_update_fastcgi_request(setting, setting_len, value);

	return SUCCESS;
}
