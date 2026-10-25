/* One startup identity per native worker, with the kernel's task-id envelope.
 * This makes CPU-share records identifiable without guessing creation order.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_WORKER_ROLE_H
#define CIUKI_DESKTOP_WORKER_ROLE_H
#ifdef __i386__
#include <ciuki/raw.h>
#include <stdint.h>
#include <stdio.h>
#endif
static inline void desk_worker_role(const char *role)
{
#ifdef __i386__
    char line[96];
    int n=snprintf(line,sizeof(line),"[desktop] worker role=%s",role);
    if (n>0 && (unsigned)n<sizeof(line))
        (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line,(uint32_t)n,0,0,0,0);
#else
    (void)role;
#endif
}
#endif
