/* fx-selftest: one unattended self-test boot (loader menu T, CBI_F_SELF_TEST).
 * The model (core/selftest_model.c) is shared by the kernel and the host
 * tests: sequencing, per-check bounds, the SELFTEST.LOG text and the serial
 * CIUKI_TEST record framing. Design: docs/design/self-test.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SELFTEST_H
#define CIUKI_SELFTEST_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SELFTEST_VERSION 1u
#define SELFTEST_FIGURES 384u   /* key=value tokens of one result line */
#define SELFTEST_ID_MAX 24u     /* check id, [a-z0-9._-] */
/* rec_emit's prefix "CIUKI_TEST v=1 run=XXXXXXXX seq=NNNNNN probe=selftest
 * event=DATA " is 65 bytes; a record above 240 bytes becomes an ERROR record.
 * Fields of one record therefore stay within 168 bytes; longer figures are
 * split into part=N continuation records (the log line keeps one line). */
#define SELFTEST_RECORD_FIELDS 168u
#define SELFTEST_LOG_BYTES 32768u
#define SELFTEST_DETAIL_BYTES 12288u
#define SELFTEST_LOG_PATH "C:/SYSTEM/LOGS/SELFTEST.LOG"

enum selftest_status { SELFTEST_PASS, SELFTEST_FAIL, SELFTEST_SKIP };
struct selftest_result {
    enum selftest_status status;
    uint32_t ms;
    char figures[SELFTEST_FIGURES];
};
struct selftest_check;
/* Each check receives its absolute deadline (ms on the ops clock) and must
 * return by it; the model still measures and fails an overrun. */
typedef void (*selftest_run_fn)(void *ctx, const struct selftest_check *, uint64_t until,
                                struct selftest_result *);
struct selftest_check {
    const char *id;
    uint32_t limit_ms;
    selftest_run_fn run;
};
struct selftest_ops {
    void *ctx;
    uint64_t (*now)(void *);                                     /* ms */
    /* Text sink: one SELFTEST.LOG line (no newline). */
    void (*line)(void *, const char *);
    /* Record sink: event and space separated key=value fields (<=168 B). */
    void (*record)(void *, const char *event, const char *fields);
    /* Before a check starts (running=id) and after it ends (running=NULL):
     * the kernel rewrites SELFTEST.LOG so a hang names the running check. */
    void (*progress)(void *, const char *running);
};
struct selftest_summary {
    unsigned passed, failed, skipped, checks;
    uint64_t started, ms;
};
/* Format helpers shared by every producer (bounded, NUL-terminated). */
void selftest_figures(struct selftest_result *, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Append one key=value token; the value is sanitized to the record grammar
 * ([A-Za-z0-9_:.,/+-]; any other byte becomes '_', empty becomes '-'). */
void selftest_token(char *figures, size_t size, const char *key, const char *value);
/* Rewrite figures in place: tokens without a valid key, reserved envelope
 * keys and repeated keys are dropped, values are sanitized, and a SKIP gets
 * reason=unspecified when it has none. */
void selftest_sanitize(char *figures, enum selftest_status);
const char *selftest_status_text(enum selftest_status);
/* Set the status and append reason=<why> (sanitized); trailing figures give
 * way so the reason always fits. why=NULL sets the status only. */
void selftest_set(struct selftest_result *, enum selftest_status, const char *why);
bool selftest_has_token(const char *figures, const char *key);

void selftest_model_begin(const struct selftest_ops *, struct selftest_summary *,
                          const char *run, const char *build, const char *version);
/* Runs count checks in order. A check that exceeds limit_ms is FAIL with
 * deadline_exceeded=1 (its figures are kept). results[i] receives each. */
void selftest_model_run(const struct selftest_ops *, const struct selftest_check *, unsigned count,
                        struct selftest_result *results, struct selftest_summary *);
/* A detail line of a check: tokens only, logged later in the Details
 * section and emitted now as event=DATA check=<id> part=detail (further
 * records of the same line: part=detail_more). */
void selftest_model_detail(const struct selftest_ops *, char *details, size_t size, size_t *used,
                           const char *id, const char *tokens);
/* Summary line, END record and the Details section (from details). */
void selftest_model_end(const struct selftest_ops *, struct selftest_summary *, const char *details);
/* "Self-test complete: N passed, M failed" (+ skipped); fits 80 bytes. */
void selftest_notice(char *out, size_t size, const struct selftest_summary *);

#ifndef SELFTEST_HOST
/* Kernel entry points (probes/selftest.c). */
struct ciuki_devices_info;
void selftest_before_desktop(void);  /* after drivers_init, before the desktop */
void selftest_after_desktop(void);   /* after supervisor_bootstrap; returns when done */
bool selftest_active(void);
/* Device inventory record 0x7F000 for the desktop (id, state, reason). */
#define SELFTEST_DEVICE_ID 0x7F000u
void selftest_device_record(struct ciuki_devices_info *);
/* Called by checks: log one detail line of the running check. */
void selftest_detail(const char *id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Desktop protocol: the desktop polls record 0x7F000 (devices inventory).
 * "launch-dosbox" asks it to start /Programs/dosbox once per generation;
 * "Self-test complete: ..." or "Self-test stalled ..." is shown as a dialog. */
void selftest_request_dosbox(void);
void selftest_device_running(void); /* back to "running" (no pending request) */
bool selftest_device_state(char *reason, size_t size, uint32_t *generation);
/* DOS full-screen checks (vm/dosvm_selftest.c), phase 2. */
void selftest_dos_window(void *, const struct selftest_check *, uint64_t, struct selftest_result *);
void selftest_dos_mode13(void *, const struct selftest_check *, uint64_t, struct selftest_result *);
void selftest_dos_vbe101(void *, const struct selftest_check *, uint64_t, struct selftest_result *);
void selftest_dos_return(void *, const struct selftest_check *, uint64_t, struct selftest_result *);
/* Check tables (core/selftest_checks.c). */
extern const struct selftest_check selftest_phase1[];
extern const unsigned selftest_phase1_count;
extern const struct selftest_check selftest_phase2[];
extern const unsigned selftest_phase2_count;
void selftest_checks_begin(void);   /* baselines (memory, serial) */
uint64_t selftest_now(void);        /* ms clock of the checks */

/* Read-only subsystem snapshots added for the self-test. Each producer is
 * a weak symbol on the consumer side: an absent subsystem reads as absent. */
struct biosvm_video_state {
    uint32_t bytes, queries, saves, restores; /* VBE 4F04 state in bytes (BX*64) */
    int query_result, save_result, restore_result;
};
void biosvm_video_state_snapshot(struct biosvm_video_state *);
struct display_selftest_info {
    uint32_t desktop_frames;  /* full desktop frames of the current generation */
    bool frame_pending;       /* a restored mode still awaits its desktop repaint */
    int last_refusal;         /* last full-screen refusal error, 0 none */
    unsigned last_provider;   /* 0 none, 1 native driver, 2 firmware video */
};
void display_selftest_info(struct display_selftest_info *);
/* The hardware 3D backend's background qualification, as its driver-private
 * owner reports it (generic display code names no chip family, DM12).
 * present: a qualified-3D backend exists for the bound adapter. */
#define RENDER3D_QUALIFICATION_PHASES 16u
struct render3d_qualification_report {
    bool present, scheduled, active, finished, qualified, fullsize, perspective, depth24, color565, lost;
    int error;
    const char *result, *reason, *marker;
    uint64_t elapsed_us, packets, triangles, copies;
    uint32_t heap_bytes, texture_bytes, required, free_vram, largest_vram, failed_phase;
    unsigned phase_count;
    struct { const char *name; uint32_t us; int error; } phases[RENDER3D_QUALIFICATION_PHASES];
};
void render3d_qualification_report(struct render3d_qualification_report *);
struct render3d_selftest {
    int error;
    bool hardware;              /* the context ran on the hardware backend */
    uint32_t expected, actual;  /* centre pixel, packed ARGB8888 */
    uint32_t elapsed_ms, width, height;
    const char *stage;
};
/* Queues one 64x64 triangle on the display worker through the production
 * render3d service with a hardware-required context, then reads it back. */
int w32_render3d_selftest(struct render3d_selftest *, uint32_t timeout_ms);
struct apm_selftest {
    bool attempted, finished, connected, capabilities_valid;
    unsigned state;             /* enum apm_state */
    uint16_t version, flags, capabilities, last_function;
    uint8_t bios_error, units;
    int error, transport_error;
    const char *reason;
};
void apm_platform_selftest(struct apm_selftest *);
#endif
#endif
