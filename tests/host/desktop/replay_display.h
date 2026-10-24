/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_REPLAY_DISPLAY_H
#define CIUKI_REPLAY_DISPLAY_H
#include <stdint.h>
#include <errno.h>
/* G10 screen/DPMS calls are unavailable in this host model. Keep raw
 * errors (rather than host errno wrappers), as desktop.c expects. */
static inline int32_t replay_DISPLAY(uint32_t op, uintptr_t arg, uint32_t d,
                                     uint32_t s, uint32_t i, uint32_t p)
{
    (void)op; (void)arg; (void)d; (void)s; (void)i; (void)p;
    return -ENOSYS;
}
#define CU_CALL(n,b,c,d,s,i,p) replay_##n(b,c,d,s,i,p)
#endif
