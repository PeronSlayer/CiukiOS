#ifndef CIUKI_SERVICES_H
#define CIUKI_SERVICES_H

#include <stdint.h>

/*
 * ciuki_services_t — function table passed by stage2 to every loaded COM.
 * COM binaries must not call stage2 functions directly (no stable ABI);
 * they must only use these pointers.
 */
typedef struct ciuki_services {
    void     (*print)(const char *s);
    void     (*print_hex64)(unsigned long long v);
    void     (*cls)(void);
} ciuki_services_t;

/* COM entry point convention */
typedef void (*com_entry_t)(void *boot_info, void *handoff, ciuki_services_t *svc);

#endif
