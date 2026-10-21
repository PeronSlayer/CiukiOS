/* Selector/parser dispatch seam; no hardware or scheduler dependencies.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SELECTOR_H
#define CIUKI_SELECTOR_H
#include <ciuki/probe.h>
#define PROBE_DOS_FREEDOS (1u << 29)
#define PROBE_DOS_UNIFIED (1u << 28)
/* f6:browser: launch the browser before the entropy pool is ready (f6-18). */
#define PROBE_BROWSER_NOW (1u << 27)
/* f6:browser url=<...>: 8..40 bytes, http:// or https:// scheme, host of
 * [A-Za-z0-9.-], optional :port, path of [A-Za-z0-9_./-]; no query. */
#define PROBE_URL_MAX 40
struct probe_selection {
    unsigned phase;
    char probe[24], run[9];
    char title[24]; /* selector-only f3:title, never included in a sweep */
    char url[PROBE_URL_MAX + 1]; /* selector-only f6:browser, fw_cfg/menu */
    uint32_t flags;
};

struct ciuki_f2_probe;
struct probe_tables {
    const struct probe_def *f0, *f1;
    unsigned f0_count, f1_count;
    const struct ciuki_f2_probe *f2;
    unsigned f2_count;
    const struct probe_def *f3;
    unsigned f3_count;
    const struct probe_def *f4; unsigned f4_count;
    const struct probe_def *f5; unsigned f5_count;
    const struct probe_def *f6; unsigned f6_count;
};
struct probe_hooks {
    void (*app_begin)(const struct probe_selection *);
    void (*app_end)(void);
    void (*panic)(void);
};
bool probes_parse_selector(const char *s, unsigned len, uint32_t boot_flags,
                           struct probe_selection *selection);
void probes_dispatch(const struct probe_selection *selection,
                     const struct probe_tables *tables, const struct probe_hooks *hooks);
#endif
