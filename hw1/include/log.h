#ifndef HW1_LOG_H
#define HW1_LOG_H

#include <stdarg.h>

void log_open(const char* path);
void log_close(void);
void log_write(const char* fmt, ...);

#endif
