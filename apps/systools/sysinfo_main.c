/* SPDX-License-Identifier: MIT
 * /Programs/sysinfo [--channel-fd=N]: System Information (Windows 98 MSInfo). */
#include "sysinfo_view.h"
#include "sysreq.h"
#include "../accessories/acc.h"
#include <ciuki/devices.h>
#include <ciuki/proc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app {
    struct du_window window; struct du_ui ui; struct sysinfo_app view;
    struct ciuki_devices_info *devices; struct si_tree *tree;
    uint32_t serial, query_serial;
    unsigned windows, windows_known;        /* desktop windows from the last DESK_WINDOWS_QUERY */
};
/* RAM from the hardware inventory (usable bytes of the boot memory map), free pages and process
 * count from call 70, User resources from the desktop's window count (cap SYSREQ_CLIENTS). */
static int collect(void *ctx, struct ciuki_devices_info *devices, struct si_input *in)
{
    struct app *a = ctx;
    devices->version = CIUKI_DEVICES_VERSION; devices->size = sizeof(*devices);
    int result = ciuki_devices(devices) ? -errno : 0;
    struct ciuki_hardware_info *h = calloc(1, sizeof(*h));
    if (h) {
        h->version = CIUKI_HARDWARE_VERSION; h->size = sizeof(*h);
        if (!ciuki_hardware_inventory(h)) {
            in->memory_total_kib = h->memory_bytes[1] / 1024;
            if (h->build_id[0]) snprintf(in->os_build, sizeof(in->os_build), "CiukiOS %.38s", h->build_id);
        }
        free(h);
    }
    struct ciuki_proc_system sys;
    unsigned gdi = 100;
    memset(&sys, 0, sizeof(sys));
    if (ciuki_proc_list(&sys, sizeof(sys)) >= 0 && sys.version == CIUKI_PROC_VERSION) {
        in->processes = sys.processes; in->memory_free_kib = (uint64_t)sys.free_pages * 4;
        if (!in->memory_total_kib) in->memory_total_kib = (uint64_t)sys.usable_pages * 4;
        if (sys.usable_pages) gdi = (unsigned)((uint64_t)sys.free_pages * 100 / sys.usable_pages);
    }
    in->gdi_free_pct = gdi;
    /* Until the desktop answers, User resources are taken as the memory share (never better than it). */
    in->user_free_pct = a->windows_known && a->windows <= SYSREQ_CLIENTS ? (SYSREQ_CLIENTS - a->windows) * 100u / SYSREQ_CLIENTS : gdi;
    return result;
}
static int launch(void *ctx, const char *program)
{ struct app *a = ctx; return sysreq_launch(&a->window, &a->serial, program); }
static int write_file(void *ctx, const char *path, const char *text, size_t length)
{
    (void)ctx;
    FILE *f = fopen(path, "wb");
    if (!f) return -errno;
    size_t n = fwrite(text, 1, length, f);
    int e = n == length ? 0 : (errno ? -errno : -EIO);
    if (fclose(f) && !e) e = errno ? -errno : -EIO;
    return e;
}
static void quit(void *ctx) { du_window_close(&((struct app *)ctx)->window); }
static void query(struct app *a)
{ int s = sysreq_windows(&a->window, &a->serial); a->query_serial = s > 0 ? (uint32_t)s : 0; }
static void on_message(struct du_window *w, const struct desk_packet *p, void *ctx)
{
    (void)w; struct app *a = ctx;
    if (p->opcode == DESK_WINDOW_INFO && p->serial == a->query_serial) {
        a->query_serial = 0; a->windows = p->count; a->windows_known = 1;
        siv_refresh(&a->view);
    } else if (p->opcode == DESK_TASK_RESULT) {
        if (p->request == DESK_WINDOWS_QUERY && p->serial == a->query_serial) a->query_serial = 0;
        else if (p->request == DESK_LAUNCH) siv_launch_result(&a->view, p->serial, p->result);
    }
}
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "sysinfo", &channel, &test, &path) || path || test) return 2;
    struct app *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    a->devices = calloc(1, sizeof(*a->devices)); a->tree = calloc(1, sizeof(*a->tree));
    if (!a->devices || !a->tree) { free(a->devices); free(a->tree); free(a); return 1; }
    if (du_window_open_resizable(&a->window, &a->ui, channel, SIV_WIDTH, SIV_HEIGHT, "System Information",
                                 SIV_MIN_WIDTH, SIV_MIN_HEIGHT)) {
        free(a->devices); free(a->tree); free(a); return 1;
    }
    a->window.user = a; a->window.message = on_message;
    struct sysinfo_host host = { a, collect, launch, write_file, quit };
    if (siv_init(&a->view, &a->ui, &host, a->devices, a->tree)) {
        du_window_destroy(&a->window); free(a->devices); free(a->tree); free(a); return 1;
    }
    query(a);
    int r = du_run(&a->window, NULL, NULL);
    du_window_destroy(&a->window);
    free(a->devices); free(a->tree); free(a);
    return r < 0 ? 1 : 0;
}
