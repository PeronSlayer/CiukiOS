/* SPDX-License-Identifier: MIT */
#include "resources.h"
#include "desktop.h"
#include "../systools/systools.h"
#include <string.h>
#ifdef __i386__
#include <ciuki/proc.h>
#include <errno.h>
static int native_pages(uint32_t *free_pages, uint32_t *usable_pages)
{
    uint8_t buffer[CIUKI_PROC_LIST_MIN];
    if (ciuki_proc_list(buffer, sizeof(buffer)) < 0) return -(errno ? errno : EIO);
    struct ciuki_proc_system sys; memcpy(&sys, buffer, sizeof(sys));
    if (sys.version != CIUKI_PROC_VERSION || sys.free_pages > sys.usable_pages) return -EIO;
    *free_pages = sys.free_pages; *usable_pages = sys.usable_pages;
    return 0;
}
int (*desk_resources_pages)(uint32_t *, uint32_t *) = native_pages;
#else
static int no_pages(uint32_t *f, uint32_t *u) { (void)f; (void)u; return -1; }
int (*desk_resources_pages)(uint32_t *, uint32_t *) = no_pages;
#endif
void desk_resources_poll(struct desktop *d)
{
    if (!d->shell) return;
    unsigned used = 0; int present = 0;
    for (int i = 0; i < DESK_CLIENTS; i++) {
        const struct desk_client *c = &d->clients[i];
        if (!c->window) continue;
        used++;
        if (!strcmp(c->application, "resmeter")) present = 1;
    }
    if (!present) {
        if (d->sh.resources.visible) { struct sh_resources off = {0}; desk_shell_resources_update(d, &off); }
        d->resources_next_ns = 0;
        return;
    }
    if (d->resources_next_ns && d->now_ns < d->resources_next_ns && d->sh.resources.visible) return;
    d->resources_next_ns = d->now_ns + DESK_RESOURCES_PERIOD_NS;
    struct rm_desktop in = { .clients_used = used, .clients_cap = DESK_CLIENTS };
    if (desk_resources_pages(&in.free_pages, &in.usable_pages)) { in.free_pages = in.usable_pages = 0; }
    struct rm_sample sample; struct rm_state state;
    rm_from_desktop(&sample, &in);
    rm_compute(&state, &sample);
    struct sh_resources now = { .visible = 1, .system = state.system_free, .user = state.user_free, .gdi = state.gdi_free, .level = state.level };
    desk_shell_resources_update(d, &now);
}
