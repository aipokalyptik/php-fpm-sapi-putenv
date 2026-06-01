#ifndef A8C_SAPI_ENV_H
#define A8C_SAPI_ENV_H

#include "php.h"

zend_result a8c_sapi_putenv_update(const char *setting, size_t setting_len);

#endif /* A8C_SAPI_ENV_H */
