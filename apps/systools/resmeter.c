/* SY-3 Resource Meter model. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <stdio.h>

void rm_compute(struct rm_state *o, const struct rm_sample *s)
{
    unsigned u = s->user_cap ? (unsigned)((uint64_t)(s->user_used > s->user_cap ? 0 : s->user_cap - s->user_used) * 100 / s->user_cap) : 0;
    unsigned g = s->gdi_cap ? (unsigned)((uint64_t)(s->gdi_used > s->gdi_cap ? 0 : s->gdi_cap - s->gdi_used) * 100 / s->gdi_cap) : 0;
    o->user_free = u; o->gdi_free = g; o->system_free = u < g ? u : g;
    o->level = o->system_free < RM_CRITICAL_PCT ? RM_CRITICAL : o->system_free < RM_LOW_PCT ? RM_LOW : RM_OK;
}
void rm_tooltip(char *out, size_t cap, const struct rm_state *s)
{
    snprintf(out, cap, "System Resources: %u%%\nUser Resources: %u%%\nGDI Resources: %u%%", s->system_free, s->user_free, s->gdi_free);
}
unsigned rm_gfsr(const struct rm_state *s, unsigned selector)
{
    /* GFSR_SYSTEMRESOURCES 0, GFSR_GDIRESOURCES 1, GFSR_USERRESOURCES 2 */
    return selector == 0 ? s->system_free : selector == 1 ? s->gdi_free : selector == 2 ? s->user_free : 0;
}
void rm_from_desktop(struct rm_sample *out, const struct rm_desktop *in)
{
    out->user_cap = in->clients_cap; out->user_used = in->clients_used;
    out->gdi_cap = in->usable_pages;
    out->gdi_used = in->usable_pages >= in->free_pages ? in->usable_pages - in->free_pages : in->usable_pages;
}
