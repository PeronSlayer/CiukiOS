/* Shared controller grammar, host dry-run and kernel call-3 observer.
 * No application stdout is consumed. SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESKTOP_V3_CONTROLLER_H
#define CIUKI_DESKTOP_V3_CONTROLLER_H
#include <stdint.h>
static const char *const desktop_v3_phases[]={"","windows","input","lease-revoke","lease-regrant","shortcuts","uninstall","crash","restart"};
struct desktop_v3_observation {uint32_t phase,generation,windows,errors,pid;};
static inline int desktop_v3_observe(struct desktop_v3_observation *last,
    const struct desktop_v3_observation *next,uint32_t child)
{
    if (!child || next->pid!=child || next->phase!=last->phase+1 || next->phase>8 ||
        !next->generation || next->generation<last->generation || next->windows<4 || next->windows>16 || next->errors)
        return 0;
    *last=*next;return 1;
}
static inline void desktop_v3_publish(const struct desktop_v3_observation *r,
    void (*emit)(const char *,const char *,const char *,...))
{
    emit("crash-isolation","DATA","case=desktop-v3 server=desktop phase=%s generation=%u windows=%u errors=%u",
        desktop_v3_phases[r->phase],r->generation,r->windows,r->errors);
}
#endif
