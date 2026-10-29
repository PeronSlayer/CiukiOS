/* fx-selftest: one unattended self-test boot (loader menu T). Phase 1 runs
 * after drivers_init and before the desktop; phase 2 after the desktop has
 * started. Report: C:\SYSTEM\LOGS\SELFTEST.LOG and CIUKI_TEST probe=selftest
 * records. The only file this boot's checks write is that log (design and
 * exceptions: docs/design/self-test.md). SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/probe.h>
#include <ciuki/storage.h>
#include <ciuki/supervisor.h>
#include <ciuki/fbdev.h>
#include <ciuki/timing.h>
#include <ciuki/selftest.h>
#include <ciuki/version.h>

#ifndef CIUKI_BUILD_HEX8
#define CIUKI_BUILD_HEX8 "00000000"
#endif
#define STALL_GRACE_MS 30000u
#define PHASE_MAX 32u

static bool active, finished, no_desktop;
static char run_id[9];
static struct selftest_summary summary;
static struct selftest_result results1[PHASE_MAX], results2[PHASE_MAX];
static char log_text[SELFTEST_LOG_BYTES];
static size_t log_used;
static bool log_truncated;
static char details[SELFTEST_DETAIL_BYTES];
static size_t details_used;
static const char *running;
static uint64_t running_since;
static uint32_t running_limit;
static bool stalled;
static char device_reason[80] = "running";
static uint32_t device_generation = 1;
static struct vfs_table log_table;
static bool flush_busy;
static int log_error;
static unsigned log_writes;
static char page[2400];
static unsigned ran[2]; /* completed checks per phase */

uint64_t selftest_now(void) { return g_ticks; }
bool selftest_active(void) { return active; }

static void gate_take(void)
{
    for (;;) {
        uint32_t f = irq_save();
        if (!flush_busy) { flush_busy = true; irq_restore(f); return; }
        irq_restore(f);
        task_sleep_ms(1);
    }
}
static void gate_drop(void) { uint32_t f = irq_save(); flush_busy = false; irq_restore(f); }

static void text_append(const char *s)
{
    size_t n = strlen(s);
    if (log_truncated) return;
    if (log_used + n + 2 + 32 >= sizeof(log_text)) { /* keep room for the marker */
        static const char cut[] = "LOG TRUNCATED\n";
        memcpy(log_text + log_used, cut, sizeof(cut));
        log_used += sizeof(cut) - 1;
        log_truncated = true;
        return;
    }
    memcpy(log_text + log_used, s, n);
    log_used += n;
    log_text[log_used++] = '\n';
    log_text[log_used] = 0;
}

/* Rewrites the whole report; a RUNNING line names the check in progress, so
 * a stall leaves its name as the file's last line. Called between checks
 * (and by the stall watchdog), never inside a display transaction. */
static void flush_file(const char *suffix)
{
    struct storage *s = storage_get();
    if (!s || !s->ready || !s->vfs.volumes[2]) { log_error = -FS_ENOENT; return; }
    int e = vfs_table_init(&s->vfs, &log_table, 2);
    if (e) { log_error = e; return; }
    int fd = vfs_open(&log_table, SELFTEST_LOG_PATH, VFS_WRITE | VFS_CREATE | VFS_TRUNCATE, VFS_DENY_WRITE, 0);
    if (fd < 0) e = fd;
    size_t done = 0;
    if (!e) e = vfs_write(&log_table, fd, log_text, log_used, &done);
    if (!e && done != log_used) e = -FS_EIO;
    if (!e && suffix && *suffix) {
        size_t n = strlen(suffix);
        e = vfs_write(&log_table, fd, suffix, n, &done);
        if (!e && done != n) e = -FS_EIO;
    }
    if (!e) e = vfs_commit(&log_table, fd);
    if (fd >= 0) { int c = vfs_close(&log_table, fd); if (!e) e = c; }
    vfs_table_destroy(&log_table);
    log_error = e;
    if (!e) log_writes++;
}

static uint64_t op_now(void *ctx) { (void)ctx; return g_ticks; }
static void op_line(void *ctx, const char *line)
{
    (void)ctx;
    gate_take(); text_append(line); gate_drop();
}
static void op_record(void *ctx, const char *event, const char *fields)
{
    (void)ctx;
    rec_emit("selftest", event, "%s", fields);
}
static void op_progress(void *ctx, const char *id)
{
    (void)ctx;
    gate_take();
    uint32_t f = irq_save();
    running = id; running_since = g_ticks; stalled = false;
    irq_restore(f);
    char suffix[64] = "";
    if (id) ksnprintf(suffix, sizeof(suffix), "RUNNING %s\n", id);
    flush_file(suffix);
    gate_drop();
    if (id) klog("[selftest] running %s", id);
}
static const struct selftest_ops ops = { 0, op_now, op_line, op_record, op_progress };

void selftest_detail(const char *id, const char *fmt, ...)
{
    if (!active || !id || !fmt) return;
    char tokens[SELFTEST_FIGURES];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(tokens, sizeof(tokens), fmt, ap);
    va_end(ap);
    gate_take();
    selftest_model_detail(&ops, details, sizeof(details), &details_used, id, tokens);
    gate_drop();
}

static void set_device(const char *reason)
{
    uint32_t f = irq_save();
    ksnprintf(device_reason, sizeof(device_reason), "%s", reason);
    if (!++device_generation) device_generation = 1;
    irq_restore(f);
}
bool selftest_device_state(char *reason, size_t size, uint32_t *generation)
{
    if (!active || !reason || !size) return false;
    uint32_t f = irq_save();
    ksnprintf(reason, size, "%s", device_reason);
    if (generation) *generation = device_generation;
    irq_restore(f);
    return true;
}
void selftest_request_dosbox(void) { set_device("launch-dosbox"); }
void selftest_device_running(void) { set_device("running"); }
/* sec-user-06: figures a session program measures (selftest.h). One latest
 * answer per name; a name is [a-z0-9_]{1,23}, its tokens the record grammar
 * (selftest_sanitize keeps only valid key=value tokens when they are shown). */
#define FIGURE_SLOTS 4u
static struct { char name[SELFTEST_ID_MAX]; char tokens[SELFTEST_FIGURES]; } figures[FIGURE_SLOTS];
void selftest_request_registry_backup(void)
{
    uint32_t f = irq_save();
    for (unsigned i = 0; i < FIGURE_SLOTS; i++)
        if (!strncmp(figures[i].name, "registry_backup", SELFTEST_ID_MAX)) figures[i].name[0] = 0;
    irq_restore(f);
    set_device("registry-backup");
}
void selftest_submit(const char *line)
{
    if (!active || finished || !line) return;
    char name[SELFTEST_ID_MAX]; unsigned n = 0;
    while (n + 1 < sizeof(name) && ((line[n] >= 'a' && line[n] <= 'z') || (line[n] >= '0' && line[n] <= '9') || line[n] == '_'))
        { name[n] = line[n]; n++; }
    name[n] = 0;
    if (!n || (line[n] && line[n] != ' ')) return;
    const char *tokens = line[n] ? line + n + 1 : "";
    uint32_t f = irq_save();
    unsigned slot = FIGURE_SLOTS;
    for (unsigned i = 0; i < FIGURE_SLOTS && slot == FIGURE_SLOTS; i++) if (!strncmp(figures[i].name, name, sizeof(name))) slot = i;
    for (unsigned i = 0; i < FIGURE_SLOTS && slot == FIGURE_SLOTS; i++) if (!figures[i].name[0]) slot = i;
    if (slot < FIGURE_SLOTS) {
        memcpy(figures[slot].name, name, n + 1);
        ksnprintf(figures[slot].tokens, sizeof(figures[slot].tokens), "%s", tokens);
    }
    irq_restore(f);
    if (slot < FIGURE_SLOTS) klog("[selftest] figure %s received", name);
}
bool selftest_figure(const char *name, char *tokens, size_t size)
{
    bool found = false;
    uint32_t f = irq_save();
    for (unsigned i = 0; i < FIGURE_SLOTS && !found; i++)
        if (figures[i].name[0] && !strncmp(figures[i].name, name, SELFTEST_ID_MAX)) {
            ksnprintf(tokens, size, "%s", figures[i].tokens); found = true;
        }
    irq_restore(f);
    if (found) set_device("running");
    return found;
}

/* Full-screen text summary when no desktop can show the notice. */
static void pin_summary(const char *title)
{
    size_t used = 0;
    int n = ksnprintf(page, sizeof(page), "%s\nReport: C:\\SYSTEM\\LOGS\\SELFTEST.LOG (run %s)\n", title, run_id);
    if (n > 0) used = (size_t)n;
    const struct selftest_result *sets[2] = { results1, results2 };
    const struct selftest_check *tables[2] = { selftest_phase1, selftest_phase2 };
    for (unsigned t = 0; t < 2; t++)
        for (unsigned i = 0; i < ran[t] && i < PHASE_MAX && used + 90 < sizeof(page); i++) {
            const struct selftest_result *r = &sets[t][i];
            char line[80];
            ksnprintf(line, sizeof(line), "%s %s %s", selftest_status_text(r->status), tables[t][i].id, r->figures);
            n = ksnprintf(page + used, sizeof(page) - used, "%s\n", line);
            if (n > 0) used += (size_t)n;
        }
    console_wake();
    console_pin_page(page);
}

static void watchdog(void *arg)
{
    (void)arg;
    while (!finished) {
        task_sleep_ms(1000);
        uint32_t f = irq_save();
        const char *id = running;
        uint64_t since = running_since;
        bool report = id && !stalled && g_ticks - since > (uint64_t)running_limit + STALL_GRACE_MS;
        if (report) stalled = true;
        irq_restore(f);
        if (!report) continue;
        char line[96], reason[80];
        ksnprintf(line, sizeof(line), "STALLED %s ms=%llu", id, (unsigned long long)(g_ticks - since));
        ksnprintf(reason, sizeof(reason), "Self-test stalled in %s", id);
        klog("[selftest] %s", line);
        rec_emit("selftest", "DATA", "check=%s status=stalled ms=%llu", id, (unsigned long long)(g_ticks - since));
        /* The stalled check holds no flush gate: append and rewrite. */
        gate_take(); text_append(line); flush_file(0); gate_drop();
        set_device(reason);
        if (!supervisor_desktop_pid()) pin_summary(reason);
    }
    for (;;) task_sleep_ms(60000);
}

static void run_phase(unsigned phase, const struct selftest_check *checks, unsigned count, struct selftest_result *results)
{
    if (count > PHASE_MAX) count = PHASE_MAX;
    for (unsigned i = 0; i < count; i++) {
        uint32_t f = irq_save(); running_limit = checks[i].limit_ms; irq_restore(f);
        selftest_model_run(&ops, &checks[i], 1, &results[i], &summary);
        ran[phase] = i + 1;
        if (!strncmp(checks[i].id, "desktop.start", SELFTEST_ID_MAX) && results[i].status != SELFTEST_PASS)
            no_desktop = true;
    }
}

void selftest_before_desktop(void)
{
    if (!(g_boot.flags & CBI_F_SELF_TEST) || active) return;
    uint64_t seed = g_cpu_tsc ? read_tsc_serialized() : g_ticks * 2654435761u;
    ksnprintf(run_id, sizeof(run_id), "%08x", (uint32_t)(seed ^ (seed >> 32)));
    rec_set_run(run_id);
    active = true;
    console_wake();
    klog("[selftest] begin run=%s build=%s (loader menu T); report %s", run_id, CIUKI_BUILD_HEX8, SELFTEST_LOG_PATH);
    selftest_model_begin(&ops, &summary, run_id, CIUKI_BUILD_HEX8, CIUKI_VERSION);
    selftest_checks_begin();
    struct task *t = task_create_kernel("selftest-watchdog", watchdog, 0, P_NORMAL);
    if (t) task_start(t);
    else klog("[selftest] watchdog unavailable: %d", -ENOMEM);
    run_phase(0, selftest_phase1, selftest_phase1_count, results1);
    /* drivers_init deferred the 2D activation so the mode checks above ran
     * without a concurrent engine bring-up (as the H page does). */
    struct storage *s = storage_get();
    if (s) fbdev_accel_schedule(&s->vfs);
}

void selftest_after_desktop(void)
{
    if (!active || finished) return;
    run_phase(1, selftest_phase2, selftest_phase2_count, results2);
    selftest_model_end(&ops, &summary, details); /* op_line takes the gate */
    char tail[96];
    ksnprintf(tail, sizeof(tail), "LOG writes=%u error=%d", log_writes + 1, log_error);
    op_line(0, tail);
    gate_take(); flush_file(0); gate_drop();
    char notice[80];
    selftest_notice(notice, sizeof(notice), &summary);
    rec_emit("selftest", "DATA", "check=selftest.log part=detail writes=%u error=%d bytes=%u",
             log_writes, log_error, (unsigned)log_used);
    klog("[selftest] %s (run %s, %llu ms, log error=%d)", notice, run_id,
         (unsigned long long)summary.ms, log_error);
    set_device(notice);
    finished = true;
    if (no_desktop || !supervisor_desktop_pid()) pin_summary(notice);
}
