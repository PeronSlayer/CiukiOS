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

#define CIUKI_F4_PROBE(id, func) static const struct probe_def \
    __attribute__((used, section(".f4probes"), aligned(4))) f4probe_##func = { id, func }
extern const struct probe_def __f4probes_start[], __f4probes_end[];
#define CIUKI_F5_PROBE(id, func) static const struct probe_def \
    __attribute__((used, section(".f5probes"), aligned(4))) f5probe_##func = { id, func }
extern const struct probe_def __f5probes_start[], __f5probes_end[];
#define CIUKI_F6_PROBE(id, func) static const struct probe_def \
    __attribute__((used, section(".f6probes"), aligned(4))) f6probe_##func = { id, func }
extern const struct probe_def __f6probes_start[], __f6probes_end[];
extern const struct probe_def __f1probes_start[], __f1probes_end[];
#define CIUKI_F3_PROBE(id, func) static const struct probe_def \
    __attribute__((used, section(".f3probes"), aligned(4))) f3probe_##func = { id, func }
extern const struct probe_def __f3probes_start[], __f3probes_end[];
/* Controller-owned, hex-framed untrusted bytes; stream is stdout/stderr/report. */
void probe_app_output(struct task *t, const char *stream, const void *bytes, uint32_t len);

/* CIUKI_TEST v=1 run=<id> seq=<n> probe=<p> event=<e> [key=value ...] */
void rec_set_run(const char *run8);
/* Runtime services emit evidence only inside a selected, begun test run. */
bool rec_active(void);
void rec_emit(const char *probe, const char *event, const char *fmt, ...);
/* Bounded runtime diagnostics, DATA framing; boot menu L only. Unlike
 * evidence, these also work without a selected/begun test run. */
void rec_diag(const char *probe, const char *fmt, ...);
void rec_emit_panicsafe(const char *probe, const char *event, const char *extra);

void probe_user_report(struct task *t, const char *msg, uint32_t len);
/* F3 failure diagnostic (f3_probes_vm.c): a DOS VM's text screen as
 * "group=screen row=N text=..." records in the record grammar. */
struct dosvm;
void probe_dos_screen(const char *probe, struct dosvm *v);
void probes_main(void *arg);      /* kernel task: parse selector, run probes */

#endif
