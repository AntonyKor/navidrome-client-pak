#include <stdarg.h>
#include <stdio.h>
void nd_log_write(int is_error, const char *fmt, ...)
{
    if (!is_error) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
