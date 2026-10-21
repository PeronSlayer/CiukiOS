#ifndef CIUKI_LWIP_STDIO_H
#define CIUKI_LWIP_STDIO_H
#include <stddef.h>
#define snprintf ksnprintf
int ksnprintf(char *, size_t, const char *, ...);
#endif
