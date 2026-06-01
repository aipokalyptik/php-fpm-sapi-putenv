#ifndef A8C_SAPI_ENV_H
#define A8C_SAPI_ENV_H

#include "php.h"

void a8c_sapi_putenv_request_init(void);
void a8c_sapi_putenv_request_shutdown(void);
zend_result a8c_sapi_putenv_update(const char *setting, size_t setting_len);

#endif /* A8C_SAPI_ENV_H */
