/* SPDX-License-Identifier: MIT
 * /Programs/resmeter [--channel-fd=N]: Resource Meter (the desktop shows a tray icon while it runs). */
#include "resmeter_view.h"
#include "sysreq.h"
#include "../accessories/acc.h"
#include <ciuki/proc.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct app {
    struct du_window window; struct du_ui ui; struct resmeter_app view;
    uint32_t serial, query_serial, free_pages, usable_pages;
};
static void finish(struct app *a, unsigned windows, int known)
{
    struct rm_desktop d = { windows, SYSREQ_CLIENTS, a->free_pages, a->usable_pages };
    rmv_update(&a->view, &d, known);
}
static void refresh(void *ctx)
{
    struct app *a = ctx; struct ciuki_proc_system sys;
    memset(&sys, 0, sizeof(sys));
    if (ciuki_proc_list(&sys, sizeof(sys)) >= 0 && sys.version == CIUKI_PROC_VERSION && sys.usable_pages) {
        a->free_pages = sys.free_pages; a->usable_pages = sys.usable_pages;
    }
    int s = sysreq_windows(&a->window, &a->serial);
    if (s > 0) a->query_serial = (uint32_t)s;
    else { a->query_serial = 0; finish(a, 0, 0); }
}
static void quit(void *ctx) { du_window_close(&((struct app *)ctx)->window); }
static void on_message(struct du_window *w, const struct desk_packet *p, void *ctx)
{
    (void)w; struct app *a = ctx;
    if (p->opcode == DESK_WINDOW_INFO && p->serial == a->query_serial) {
        a->query_serial = 0; finish(a, p->count, 1);          /* every window is one client slot */
    } else if (p->opcode == DESK_TASK_RESULT && p->request == DESK_WINDOWS_QUERY && p->serial == a->query_serial) {
        a->query_serial = 0; finish(a, 0, 0);                  /* refused or busy: User unavailable */
    }
}
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "resmeter", &channel, &test, &path) || path || test) return 2;
    struct app *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    if (du_window_open(&a->window, &a->ui, channel, RMV_WIDTH, RMV_HEIGHT, "Resource Meter")) { free(a); return 1; }
    a->window.user = a; a->window.message = on_message;
    struct resmeter_host host = { a, refresh, quit };
    if (rmv_init(&a->view, &a->ui, &host)) { du_window_destroy(&a->window); free(a); return 1; }
    refresh(a);
    int r = du_run(&a->window, NULL, NULL);
    du_window_destroy(&a->window);
    free(a);
    return r < 0 ? 1 : 0;
}
