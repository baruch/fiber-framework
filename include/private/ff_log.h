#ifndef FF_LOG_PRIVATE_H
#define FF_LOG_PRIVATE_H

#include "ff/ff_log.h"

#ifdef __cplusplus
extern "C" {
#endif

void ff_log_initialize(const wchar_t *log_filename);

void ff_log_shutdown();

#ifdef __cplusplus
}
#endif

#endif
