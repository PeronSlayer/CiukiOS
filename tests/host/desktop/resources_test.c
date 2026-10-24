/* G30 Resource Meter tray feed: visible only while a resmeter client has a window,
 * sampled every five seconds, thresholds from the shared model. SPDX-License-Identifier: MIT */
#include "../../../apps/desktop/resources.h"
#include "../../../apps/desktop/desktop.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct sh_resources last; static unsigned updates;
void desk_shell_resources_update(struct desktop *d, const struct sh_resources *r)
{ d->sh.resources = *r; last = *r; updates++; }
static uint32_t free_now = 800, usable = 1000; static unsigned page_calls; static int page_fail;
static int pages(uint32_t *f, uint32_t *u) { page_calls++; if (page_fail) return -5; *f = free_now; *u = usable; return 0; }
static struct desktop d;
static void window(int slot, const char *application) { d.clients[slot].window = 1; snprintf(d.clients[slot].application, sizeof(d.clients[slot].application), "%s", application); }
int main(void)
{
    desk_resources_pages = pages; d.shell = 1;
    window(0, "files"); desk_resources_poll(&d);
    assert(!updates && !d.sh.resources.visible);                    /* no resmeter client: nothing shown */
    window(1, "resmeter"); d.now_ns = 1000;
    desk_resources_poll(&d);
    assert(updates == 1 && last.visible && last.user == 87 && last.gdi == 80 && last.system == 80 && last.level == 0);   /* 2 of 16 slots, 800 of 1000 pages free */
    desk_resources_poll(&d); assert(updates == 1 && page_calls == 1);                                              /* sampled once per period */
    d.now_ns += DESK_RESOURCES_PERIOD_NS; free_now = 100; desk_resources_poll(&d);
    assert(updates == 2 && last.gdi == 10 && last.system == 10 && last.level == 2 && page_calls == 2);          /* critical */
    d.now_ns += DESK_RESOURCES_PERIOD_NS; free_now = 250; for (int i = 2; i < 14; i++) window(i, "calc");
    desk_resources_poll(&d); assert(last.user == 12 && last.system == 12 && last.level == 2);                                            /* 14 of 16 slots */
    d.now_ns += DESK_RESOURCES_PERIOD_NS; page_fail = 1; desk_resources_poll(&d);
    assert(last.visible && last.gdi == 0);                                                                         /* unreadable pages: reported as exhausted, not faked */
    d.clients[1].window = 0; desk_resources_poll(&d);
    assert(!last.visible && !d.sh.resources.visible && d.resources_next_ns == 0);                                  /* the program ended */
    unsigned before = updates; desk_resources_poll(&d); assert(updates == before);
    puts("PASS G30 Resource Meter tray feed");
    return 0;
}
