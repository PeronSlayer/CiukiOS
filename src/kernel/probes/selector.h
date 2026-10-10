/* Selector/parser dispatch seam; no hardware or scheduler dependencies.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SELECTOR_H
#define CIUKI_SELECTOR_H
#include <ciuki/probe.h>
struct probe_selection {
    unsigned phase;
    char probe[24], run[9];
    uint32_t flags;
};

struct ciuki_f2_probe;
struct probe_tables {
    const struct probe_def *f0, *f1;
    unsigned f0_count, f1_count;
    const struct ciuki_f2_probe *f2;
    unsigned f2_count;
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
