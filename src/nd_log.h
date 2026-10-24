#pragma once
#include <stdarg.h>

void nd_log_init(void);
void nd_log_write(int is_error, const char *fmt, ...);
void nd_log_flush(void);

/* Info-level: buffered, no flush. */
#define ND_LOG(fmt, ...)       nd_log_write(0, fmt, ##__VA_ARGS__)
/* Error-level: buffered + immediate flush so errors survive crashes. */
#define ND_LOG_ERROR(fmt, ...) nd_log_write(1, fmt, ##__VA_ARGS__)
