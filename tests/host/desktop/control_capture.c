/* Control Panel pages rendered on the host (review renders; never guest evidence).
 * usage: control_capture portrait.xrgb out.xrgb page
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "display_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char saved[CFG_TEXT_MAX]; static size_t saved_n;
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; if (!saved_n || saved_n > s) return -1; memcpy(t, saved, saved_n); *n = saved_n; return 0; }
static int save(void *c, const char *t, size_t n) { (void)c; if (n > sizeof(saved)) return -1; memcpy(saved, t, n); saved_n = n; return 0; }
static int mixer(void *c, int *v, int *m) { (void)c; *v = 70; *m = 0; return 0; }
static int apply(void *c, unsigned r, int v, int m) { (void)c; (void)r; (void)v; (void)m; return 0; }
static uint64_t now(void *c) { (void)c; return 1791763200ull + 20 * 3600 + 46 * 60 + 12; }
static int display(void *c, struct ciuki_display_info *d) { (void)c; display_fixture(d); return 0; }
int main(int argc, char **argv)
{
    assert(argc == 4);
    static uint32_t portrait[256 * 256], pixels[CP_WIDTH * CP_HEIGHT];
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    static struct cp_app a; static struct du_ui u;
    a.host = (struct cp_host){.load = load, .save = save, .mixer = mixer, .apply = apply, .now = now, .display = display, .portrait = portrait,
                              .screen_w = 1024, .screen_h = 768, .version = "0.9.0-prealpha", .build = "host-preview",
                              .machine = "i686", .mem_total_k = 511u * 1024, .mem_free_k = 480u * 1024, .processes = 6};
    du_init(&u, pixels, CP_WIDTH, CP_HEIGHT);
    u.focused_window = 1;
    assert(!cp_attach(&a, &u, cp_page_index(argv[3])));
    du_render(&u);
    f = fopen(argv[2], "wb"); assert(f && fwrite(pixels, 4, CP_WIDTH * CP_HEIGHT, f) == CP_WIDTH * CP_HEIGHT); fclose(f);
    cp_detach(&a);
    return 0;
}
