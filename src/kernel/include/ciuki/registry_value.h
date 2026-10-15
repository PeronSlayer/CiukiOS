/* Read-only boot settings through the shared registry parser. */
#ifndef CIUKI_REGISTRY_VALUE_H
#define CIUKI_REGISTRY_VALUE_H
#include <stdint.h>
#include <stdbool.h>
struct vfs;
bool kernel_registry_int(struct vfs *, const char *, const char *, int32_t *) __attribute__((weak));
#endif
