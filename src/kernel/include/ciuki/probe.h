/* F0/F1 probes and evidence records (docs/design/f0-acceptance.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PROBE_H
#define CIUKI_PROBE_H

#include <stdint.h>
#include <stdbool.h>

struct task;

struct probe_def { const char *name; int (*fn)(void); };
#define CIUKI_F1_PROBE(id, func) static const struct probe_def \
    __attribute__((used, section(".f1probes"))) f1probe_##func = { id, func }

extern const struct probe_def __f1probes_start[], __f1probes_end[];

/* CIUKI_TEST v=1 run=<id> seq=<n> probe=<p> event=<e> [key=value ...] */
void rec_set_run(const char *run8);
void rec_emit(const char *probe, const char *event, const char *fmt, ...);
void rec_emit_panicsafe(const char *probe, const char *event, const char *extra);

void probe_user_report(struct task *t, const char *msg, uint32_t len);
void probes_main(void *arg);      /* kernel task: parse selector, run probes */

#endif
