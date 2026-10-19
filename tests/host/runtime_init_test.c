/* Fake boot calls production activation/safe evidence; no hardware access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/init.h>
#include <ciuki/fbdev.h>
#include <ciuki/i8042.h>
#include <ciuki/biosvm.h>
#include <ciuki/fwinput.h>
#include <ciuki/registry.h>
#include <ciuki/ata.h>
#include <ciuki/probe.h>

/* Compile the actual boot-only section of probes.c with host boundaries.
 * The rest of that TU contains privileged instructions and unrelated probes.
 * Generate only under build/host, and inherit the same sanitizer flags. */
#ifndef RUNTIME_BOOT_TEST
#include <unistd.h>
#include <sys/wait.h>

static int child_status(pid_t child)
{
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

int main(void)
{
    char root[1024], source[1200], include[1200], output[1200], headers[1200];
    if (snprintf(root, sizeof(root), "%s", __FILE__) >= (int)sizeof(root)) return 1;
    char *suffix = strstr(root, "/tests/host/runtime_init_test.c");
    if (!suffix) return 1; /* host_kernel_tests.sh supplies the absolute path */
    *suffix = 0;
    snprintf(source, sizeof(source), "%s/src/kernel/probes/probes.c", root);
    snprintf(include, sizeof(include), "%s/build/host/runtime_boot_probe.inc", root);
    snprintf(output, sizeof(output), "%s/build/host/runtime_boot_test", root);
    snprintf(headers, sizeof(headers), "%s/src/kernel/include", root);
    FILE *in = fopen(source, "r"), *out = fopen(include, "w");
    if (!in || !out) { perror("boot fixture"); return 1; }
    char line[1024];
    bool copying = false, boot = false, complete = false;
    while (fgets(line, sizeof(line), in)) {
        if (!strcmp(line, "static uint64_t installed_ram_bytes(void)\n")) copying = true;
        if (!strcmp(line, "static int probe_boot(void)\n")) boot = true;
        if (copying && fputs(line, out) == EOF) break;
        if (boot && !strcmp(line, "}\n")) { complete = true; break; }
    }
    bool read_ok = !ferror(in);
    fclose(in);
    if (fclose(out) || !read_ok || !complete) return 1;
    snprintf(source, sizeof(source), "%s/src/kernel/fs/mount.c", root);
    snprintf(include, sizeof(include), "%s/build/host/runtime_storage_init.inc", root);
    in = fopen(source, "r"); out = fopen(include, "w");
    if (!in || !out) return 1;
    copying = complete = false;
    while (fgets(line, sizeof(line), in)) {
        if (!strcmp(line, "void storage_init(void)\n")) copying = true;
        if (copying && fputs(line, out) == EOF) break;
        if (copying && !strcmp(line, "}\n")) { complete = true; break; }
    }
    read_ok = !ferror(in); fclose(in);
    if (fclose(out) || !read_ok || !complete) return 1;
    snprintf(source, sizeof(source), "%s/src/kernel/usb/usb_core.c", root);
    snprintf(include, sizeof(include), "%s/build/host/runtime_usb_policy.inc", root);
    in = fopen(source, "r"); out = fopen(include, "w");
    if (!in || !out) return 1;
    copying = complete = false;
    while (fgets(line, sizeof(line), in)) {
        if (!strcmp(line, "static bool blocked(void)\n")) copying = true;
        if (copying && fputs(line, out) == EOF) break;
        if (copying && strchr(line, '}')) { complete = true; break; }
    }
    read_ok = !ferror(in); fclose(in);
    if (fclose(out) || !read_ok || !complete) return 1;
    pid_t child = fork();
    if (!child) {
        execlp("clang", "clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-DRUNTIME_BOOT_TEST", "-I", headers,
               __FILE__, "-o", output, (char *)0);
        _exit(1);
    }
    int status = child_status(child);
    if (status) return status;
    child = fork();
    if (!child) { execl(output, output, (char *)0); _exit(1); }
    return child_status(child);
}
#else
static unsigned failures, calls, records, consoles, optional_records, logs;
bool probes_operator_mode;
static unsigned sequence, frame_count, max_activation_length;
static char frames[64][512];
static char log_lines[64][256];
volatile uint64_t g_ticks;
uint32_t ticks_lo(void) { return (uint32_t)g_ticks; }
char g_cpu_vendor[13] = "GenuineIntel";
uint32_t g_cpu_signature = 0x000006B1u;
#define CIUKI_BUILD_HEX8 "12345678"
#define CIUKI_BUILD_DIRTY 1
#define PIT_DIVISOR 1193u
uint32_t pmm_total_usable(void) { return 32768; }

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
void task_sleep_ms(uint32_t ms) { CHECK(ms == 10010); g_ticks += ms; }
static char order[16], provenance[256];
static uint32_t expected_flags;
static int fb_result, input_result, bios_result, adapter_result, ata_result;
static bool usb_blocked;
static bool disk, quarantined, fail_expected;
static struct fb_device display;
struct ciuki_boot_info g_boot;

static void called(char id)
{
    CHECK(g_boot.flags == expected_flags && calls + 1 < sizeof(order));
    order[calls++] = id;
    order[calls] = 0;
    g_ticks += 7;
}
int fbdev_init(void) { called('F'); display.present = !fb_result && !!g_boot.fb_phys; return fb_result; }
const struct fb_device *fbdev_get(void) { return &display; }
int i8042_init(void) { called('N'); return input_result; }
int biosvm_init(void) { called('B'); return bios_result; }
int fwinput_adapter_init(void) { called('W'); return adapter_result; }
int biosvm_input_fallback(void) { CHECK(drivers_input_firmware()); called('H'); return 0; }
#include "../../build/host/runtime_usb_policy.inc"
void usb_init(void) { usb_blocked = blocked(); }
int ata_init(void) { called('A'); return ata_result; }

struct ata_device *ata_device_get(unsigned c, unsigned u)
{
    static struct ata_device d;
    return disk && !c && !u ? &d : 0;
}
unsigned registry_count(void) { return quarantined ? 1 : 0; }
const struct resource *registry_get(unsigned i)
{
    static struct resource r = { .owner = "ata0", .state = RS_QUARANTINED };
    CHECK(!i);
    return &r;
}
void i8042_snapshot(struct i8042_stats *out)
{
    *out = (struct i8042_stats){ .active = drivers_input_firmware() ? !bios_result && !adapter_result : !input_result };
}
void fwinput_backend_state(struct fwinput_backend_state *out)
{
    *out = (struct fwinput_backend_state){ .keyboard = true, .mouse = true, .key_releases = true };
}
void console_write(const char *s, size_t n) { CHECK(s && n); consoles++; }
void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    CHECK(logs < ARRAY_SIZE(log_lines));
    if (logs < ARRAY_SIZE(log_lines)) vsnprintf(log_lines[logs], sizeof(log_lines[logs]), fmt, ap);
    va_end(ap);
    logs++;
}
void rec_set_run(const char *run8)
{
    CHECK(!strcmp(run8, "12ab34cd"));
    sequence = frame_count = optional_records = 0;
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[512] = { 0 };
    va_list ap;
    va_start(ap, fmt);
    if (fmt) vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    if (!frame_count) CHECK(!strcmp(event, "BEGIN"));
    CHECK(frame_count < ARRAY_SIZE(frames));
    char line[512];
    int n = snprintf(line, sizeof(line), "CIUKI_TEST v=1 run=12ab34cd seq=%06u probe=%s event=%s%s%s",
                     ++sequence, probe, event, *extra ? " " : "", extra);
    CHECK(n > 0 && n < (int)sizeof(line));
    if (frame_count < ARRAY_SIZE(frames)) strcpy(frames[frame_count++], line);
    size_t length = (size_t)n;
    CHECK(length <= 240);
    if (length > 240) printf("oversized %s %s %s\n", probe, event, extra);
    if (strstr(extra, "case=option")) snprintf(provenance, sizeof(provenance), "%s", extra);
    if (!strcmp(probe, "boot") && strstr(extra, "group=activation device=")) {
        optional_records++;
        if (length > max_activation_length) max_activation_length = (unsigned)length;
    }
    if (!strcmp(event, "END")) CHECK(!!strstr(extra, "status=FAIL") == fail_expected);
    records++;
}

#include <ciuki/storage.h>
#include <ciuki/bootlog.h>
#include <ciuki/work.h>
static struct storage system_storage;
static bool initialized;
static void storage_timer(void *arg) { (void)arg; CHECK(false); }
void fs_calendar_init(void) { called('S'); }
void ledger_snapshot(struct mm_ledger *out) {
    memset(out,0,sizeof(*out)); out->zone[ZONE_NORMAL].usable=28672;
    out->zone[ZONE_NORMAL].free=24576; out->high=2048;
}
uint32_t cache_initial_bytes(uint32_t usable,uint32_t free_pages,uint32_t high) {
    CHECK(usable==28672 && free_pages==24576 && high==2048); return CACHE_DEFAULT_BYTES;
}
int cache_adaptive_enable(struct block_cache *cache,uint32_t usable) {
    CHECK(cache==&system_storage.cache && usable==28672); cache->adaptive=true; return 0;
}
int storage_setup(struct storage *s, size_t bytes) { CHECK(bytes==CACHE_DEFAULT_BYTES); memset(s, 0, sizeof(*s)); s->ready = true; return 0; }
bool storage_probe_readonly(const char *selector, unsigned length) { return selector && length; }
int kwork_init(void) { return 0; }
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{ (void)name; (void)fn; (void)arg; (void)prio; static struct task task; return &task; }
void task_start(struct task *task) { CHECK(task != 0); }
int bootlog_activate(struct vfs *vfs, bool qualified, uint32_t seq)
{ (void)vfs; (void)qualified; (void)seq; return 0; }
int storage_pagefile_bind(struct storage *s, unsigned drive, const char *path)
{ CHECK(s && drive == 2 && path); return 0; }
int atapi_ready(struct ata_device *d) { (void)d; return -FS_EIO; }
int optical_bind(struct storage *s,unsigned slot,struct ata_device *d)
{ (void)s; (void)slot; (void)d; CHECK(false); return -FS_EIO; }
void optical_service(struct storage *s,uint64_t now) { (void)s; (void)now; }
void optical_start_service(struct storage *s) { (void)s; }
int storage_add_ro(struct storage *s, unsigned slot, struct blkdev *b, uint64_t session)
{ (void)s; (void)slot; (void)b; (void)session; return -FS_EIO; }
int storage_add_disk(struct storage *s, unsigned slot, struct blkdev *dev)
{
    (void)dev; CHECK(!slot);
    s->volumes[2] = (struct storage_volume){ .present = true, .drive = 2,
        .partition = 1, .read_gate = true, .read_sequence = 1,
        .fat = { .readonly = true, .type = 32 } };
    return 0;
}
struct storage_volume *storage_volume(struct storage *s, unsigned drive)
{ return s->volumes[drive].present ? &s->volumes[drive] : 0; }
int storage_enable_write(struct storage *s, unsigned drive)
{ s->volumes[drive].fat.readonly = false; return 0; }
uint32_t rec_premature_records(void) { return 0; }
#include "../../src/kernel/core/init.c"
#include "../../build/host/runtime_storage_init.inc"
static struct storage host_storage;
struct storage *storage_get(void) { return &host_storage; }
void file_clock_start(int64_t build_epoch, bool rtc_qualified)
{
    CHECK(build_epoch == 0 && rtc_qualified);
}
static bool files_ready;
int files_bootstrap(struct vfs *vfs) { CHECK(vfs); files_ready=true; return 0; }
void fbdev_accel_schedule(struct vfs *vfs)
{
    CHECK(vfs==&storage_get()->vfs && state.initialized && files_ready);
    CHECK(state.ata_sequence==4 && drivers_activation_count()>=3);
}
int probe_bootlog(void) { CHECK(false); return 1; } /* registration only */
#include "../../src/kernel/probes/safe_probe.c"

static int verdict(const char *probe, bool ok, const char *reason)
{
    if (ok) rec_emit(probe, "END", "status=PASS");
    else rec_emit(probe, "END", "status=FAIL reason=%s", reason);
    return ok ? 0 : 1;
}
#include "../../build/host/runtime_boot_probe.inc"

static void reset(uint32_t flags, bool firmware, bool lfb)
{
    if (firmware) flags |= CBI_F_INPUT_FORCED;
    probes_operator_mode = false;
    state = (struct drivers_state){ 0 };
    memset(activation_ledger, 0, sizeof(activation_ledger));
    activation_count = 0; initialized = false;
    g_ticks = 100;
    CHECK(!drivers_activation_ordered() && !drivers_activation_get(0));
    rec_set_run("12ab34cd");
    g_boot = (struct ciuki_boot_info){ .flags = flags, .input_policy = firmware ? CBI_INPUT_FIRMWARE : CBI_INPUT_NATIVE,
                                     .fb_phys = lfb ? 0xE0000000u : 0 };
    expected_flags = flags;
    fb_result = input_result = bios_result = adapter_result = ata_result = 0;
    disk = quarantined = fail_expected = false;
    calls = optional_records = consoles = logs = 0;
    files_ready = false;
    usb_blocked = false;
    order[0] = provenance[0] = 0;
    display = (struct fb_device){ 0 };
}
static void verify(const char *expected, unsigned optional, const char *fb, const char *input, const char *ata, unsigned failed)
{
    unsigned before = records;
    drivers_init();
    CHECK(records == before && !frame_count && !sequence); /* ordinary boot: klog only */
    CHECK(logs >= 5 && strstr(log_lines[0], "[init] flag "));
    bool input_log = false, ata_log = false;
    for (unsigned i = 0; i < logs; i++) {
        input_log |= strstr(log_lines[i], "[init] input result=") != 0;
        ata_log |= strstr(log_lines[i], "[init] ata ") != 0;
    }
    CHECK(input_log && ata_log);
    char with_storage[16]; snprintf(with_storage,sizeof(with_storage),"%sS",expected);
    CHECK(!strcmp(order, with_storage));
    struct drivers_state s;
    drivers_snapshot(&s);
    CHECK(s.initialized && s.boot_flags == expected_flags && s.optional_activations == optional);
    CHECK(s.flag_sequence < s.fb_sequence && s.fb_sequence < s.input_sequence && s.input_sequence < s.ata_sequence);
    unsigned entries = disk ? 6 : 4;
    CHECK(drivers_activation_count() == entries && drivers_activation_ordered());
    CHECK(!drivers_activation_get(entries) && !drivers_activation_get(UINT32_MAX));
    const char *devices[] = { "framebuffer", "input", "ata" };
    const char *results[] = { fb, input, ata };
    const int errors[] = { fb_result, state.input_error, ata_result };
    CHECK(usb_blocked == state.firmware);
    struct activation_entry saved[3];
    uint64_t previous = 100;
    for (unsigned i = 0; i < ARRAY_SIZE(saved); i++) {
        const struct activation_entry *e = drivers_activation_get(i);
        CHECK(e && e->seq == i + 2 && !strcmp(e->device, devices[i]) && !strcmp(e->result, results[i]));
        CHECK(e->tick >= previous && e->tick <= g_ticks && e->safe_flag_before == s.safe);
        CHECK(e->error == errors[i] && e->reason && *e->reason);
        saved[i] = *e;
        previous = e->tick;
    }
    CHECK(!strcmp(saved[0].reason, state.fb_called ? "boot_console" : "safe_text_console"));
    CHECK(saved[0].required == state.safe && saved[1].required);
    CHECK(!strcmp(saved[1].reason, state.input_fallback ? "firmware_failed_native_fallback" : state.firmware ? "firmware" : "native"));
    CHECK(!strcmp(saved[2].reason, state.safe ? "safe_mode" : "native_discovery"));
    CHECK(saved[2].present == (disk && state.ata_called) && saved[2].quarantined == (quarantined && state.ata_called));
    struct activation_entry ledger[ACTIVATION_LEDGER_MAX];
    previous = 0;
    for (unsigned i = 0; i < entries; i++) {
        const struct activation_entry *e = drivers_activation_get(i);
        CHECK(e->seq == i + 2 && e->tick >= previous && e->safe_flag_before == s.safe);
        if (e->kind != ACTIVATION_DEVICE) CHECK(e->reason && !e->qualified && !e->writes);
        ledger[i] = *e;
        previous = e->tick;
    }
    unsigned n = calls, r = records, saved_logs = logs;
    drivers_init();
    CHECK(calls == n && records == r && logs == saved_logs && drivers_activation_count() == entries);
    for (unsigned i = 0; i < ARRAY_SIZE(saved); i++)
        CHECK(!memcmp(&saved[i], drivers_activation_get(i), sizeof(saved[i])));
    for (unsigned i = 0; i < entries; i++)
        CHECK(!memcmp(&ledger[i], drivers_activation_get(i), sizeof(ledger[i])));
    bool safe_failure = fail_expected;
    fail_expected = false; /* boot tests timer progress even if a driver failed */
    CHECK(!probe_boot());
    fail_expected = safe_failure;
    unsigned storage_records = disk ? 4 : 1;
    CHECK(frame_count == 12 + storage_records && optional_records == 3 && sequence == frame_count);
    CHECK(!strcmp(frames[0], "CIUKI_TEST v=1 run=12ab34cd seq=000001 probe=boot event=BEGIN"));
    CHECK(strstr(frames[1], "group=activation_flag activation_seq=1"));
    for (unsigned i = 0; i < ARRAY_SIZE(saved); i++) {
        char fields[128];
        snprintf(fields, sizeof(fields), "group=activation device=%s result=%s error=%d activation_seq=%u",
                 devices[i], results[i], saved[i].error, saved[i].seq);
        CHECK(strstr(frames[i + 2], fields));
    }
    char summary[128];
    snprintf(summary, sizeof(summary), "group=activation_summary devices=3 failures=%u optional_activations=%u", failed, optional);
    CHECK(strstr(frames[5 + storage_records], summary));
    CHECK(strstr(frames[5 + storage_records], "premature_records=0"));
    CHECK(strstr(frames[5], disk ? "group=storage disk=0 result=0 gate=read writes=0" : "group=storage_identity"));
    if (disk) {
        CHECK(drivers_mount_get(2) && drivers_mount_get(2)->partition == 1);
        CHECK(strstr(frames[6], "group=mount drive=C disk=0 part=1"));
        CHECK(strstr(frames[7], "group=storage drive=C"));
    }
    CHECK(strstr(frames[6 + storage_records], "group=boot cpuid=") && strstr(frames[7 + storage_records], "group=boot unexpected_resets="));
    CHECK(strstr(frames[8 + storage_records], "group=video ") && strstr(frames[9 + storage_records], "event=READY ") &&
          strstr(frames[10 + storage_records], "ready_tick=") && strstr(frames[11 + storage_records], "event=END status=PASS"));
    CHECK(calls == n && logs == saved_logs); /* replay never reactivates hardware */
    for (unsigned i = 0; i < entries; i++)
        CHECK(!memcmp(&ledger[i], drivers_activation_get(i), sizeof(ledger[i])));
}
static int safe_run(void)
{
    rec_set_run("12ab34cd");
    int result = probe_safe();
    CHECK(strstr(frames[0], "seq=000001 probe=safe event=BEGIN"));
    CHECK(!optional_records);
    for (unsigned i = 0; i < frame_count; i++) CHECK(!strstr(frames[i], "probe=boot"));
    return result;
}

static void input_decision_table(void)
{
    unsigned rows = 0;
    for (unsigned policy = 0; policy < 2; policy++)
    for (unsigned forced = 0; forced < 2; forced++)
    for (unsigned detected = 0; detected < 2; detected++)
    for (unsigned safe = 0; safe < 2; safe++)
    for (unsigned bios_fail = 0; bios_fail < 2; bios_fail++)
    for (unsigned adapter_fail = 0; adapter_fail < 2; adapter_fail++)
    for (unsigned native_fail = 0; native_fail < 2; native_fail++) {
        reset((forced ? CBI_F_INPUT_FORCED : 0) | (detected ? CBI_F_INPUT_DETECTED : 0) |
              (safe ? CBI_F_SAFE_MODE : 0), false, true);
        g_boot.input_policy = policy;
        bios_result = bios_fail ? -14 : 0;
        adapter_result = adapter_fail ? -12 : 0;
        input_result = native_fail ? -5 : 0;
        struct ciuki_boot_info request = g_boot;
        bool first_firmware = forced || detected || safe;
        bool fallback = first_firmware && (bios_fail || adapter_fail);
        bool firmware = first_firmware && !fallback;
        int error = firmware ? 0 : input_result;
        CHECK(drivers_input_firmware() == first_firmware);
        drivers_init();
        CHECK(!memcmp(&request, &g_boot, sizeof(request)));
        CHECK(state.firmware == firmware && drivers_input_firmware() == firmware);
        CHECK(state.input_fallback == fallback && state.input_error == error);
        CHECK(state.firmware_error == (fallback ? (bios_fail ? bios_result : adapter_result) : 0));
        CHECK(usb_blocked == firmware && drivers_activation_ordered());
        const struct activation_entry *e = drivers_activation_get(1);
        CHECK(e && e->seq == 3 && e->required && e->safe_flag_before == !!safe);
        CHECK(e->error == error && !strcmp(e->result, error ? "failed" : "ready"));
        CHECK(!strcmp(e->reason, fallback ? "firmware_failed_native_fallback" : firmware ? "firmware" : "native"));
        char expected[16] = "F";
        strcat(expected, !first_firmware ? "N" : !fallback ? "BW" : bios_fail ? "BHN" : "BWHN");
        if (!safe) strcat(expected, "A");
        strcat(expected, "S");
        CHECK(!strcmp(order, expected));
        bool unknown = false;
        for (unsigned i = 0; i < logs; i++)
            unknown |= strstr(log_lines[i], "[init] input_policy=firmware source=unknown action=native") != 0;
        CHECK(unknown == (policy && !first_firmware));
        unsigned before = calls;
        drivers_init();
        CHECK(calls == before);
        rows++;
    }
    CHECK(rows == 128);
    printf("input decision table: %u rows (policy/forced/detected/safe/BIOS/adapter/native, USB, ledger, request preserved) PASS\n", rows);
}

static void safe_sweep_tests(void)
{
    reset(0, false, true); disk = true;
    verify("FNA", 2, "ready", "ready", "ready", 0);
    probes_operator_mode = true;
    unsigned before = calls;
    CHECK(safe_run() == -ECANCELED && calls == before && consoles == 1);
    CHECK(strstr(provenance, "safe_mode=0 fw_cfg=0 boot_cfg=0 menu=0"));
    CHECK(strstr(frames[frame_count - 3], "event=READY console=1 input=1 optional_activations=2"));
    CHECK(strstr(frames[frame_count - 2], "event=DATA subcase=required status=not_run reason=safe_mode_not_selected"));
    CHECK(strstr(frames[frame_count - 1], "event=END status=NOT_RUN reason=safe_mode_not_selected"));
    CHECK(strstr(frames[frame_count - 4], "errors=1")); /* original observations retained */
    probes_operator_mode = false; fail_expected = true;
    CHECK(safe_run() == 1 && strstr(frames[frame_count - 1], "status=FAIL reason=safe_contract"));
    /* A positive source with a missing flag is a broken safe boot, even
     * during a sweep; it must not be mistaken for unselected mode. */
    probes_operator_mode = true;
    memcpy(g_boot.options, "safe=1", 7);
    CHECK(safe_run() == 1 && strstr(frames[frame_count - 1], "status=FAIL"));
    reset(CBI_F_SAFE_MODE, false, true);
    verify("FBW", 0, "ready", "ready", "disabled", 0);
    probes_operator_mode = true;
    CHECK(!safe_run() && strstr(frames[frame_count - 1], "status=PASS"));
    reset(CBI_F_SAFE_MODE, false, false); bios_result = input_result = -5; fail_expected = true;
    verify("BHN", 0, "disabled", "failed", "disabled", 1);
    probes_operator_mode = true;
    CHECK(safe_run() == 1 && strstr(frames[frame_count - 1], "status=FAIL"));
}

int main(void)
{
    CHECK(CIUKI_BUILD_EPOCH == 0); /* the host build uses the header fallback */
    input_decision_table();
    safe_sweep_tests();
    reset(0, false, true); disk = true; verify("FNA", 2, "ready", "ready", "ready", 0);
    reset(CBI_F_TEXT_MODE, false, false); verify("FNA", 2, "ready", "ready", "absent", 0);
    reset(0, false, true); fb_result = -EINVAL; input_result = -5; ata_result = -5; verify("FNA", 2, "failed", "failed", "failed", 3);
    CHECK(state.fb_error == -EINVAL && state.input_error == -5 && state.ata_error == -5);
    reset(0, false, true); quarantined = true; verify("FNA", 2, "ready", "ready", "failed", 1);
    reset(0, false, true); disk = quarantined = true; verify("FNA", 2, "ready", "ready", "failed", 1);
    reset(0, true, true); adapter_result = -5; verify("FBWHNA", 2, "ready", "ready", "absent", 0);
    reset(0, false, true); fb_result = input_result = ata_result = INT32_MIN;
    verify("FNA", 2, "failed", "failed", "failed", 3);
    reset(0, true, true); verify("FBWA", 2, "ready", "ready", "absent", 0);
    reset(0, true, true); bios_result = -5; verify("FBHNA", 2, "ready", "ready", "absent", 0);
    reset(CBI_F_SAFE_MODE, false, true); verify("FBW", 0, "ready", "ready", "disabled", 0); CHECK(!safe_run() && consoles == 1);
    CHECK(strstr(provenance, "menu=1 menu_inferred=1"));
    reset(CBI_F_SAFE_MODE | CBI_F_TEXT_MODE, false, false);
    memcpy(g_boot.options, "video=640x480\nsafe=1\n", sizeof("video=640x480\nsafe=1\n"));
    verify("BW", 0, "disabled", "ready", "disabled", 0); CHECK(!safe_run() && consoles == 1 && strstr(provenance, "boot_cfg=1 menu=0"));
    reset(CBI_F_SAFE_MODE | CBI_F_TEXT_MODE | CBI_F_SMBIOS_QEMU | CBI_F_TEST_REQUEST | CBI_F_INPUT_FORCED,
          true, false);
    const char *selector = "f1:safe run=12ab34cd platform=e500 safe=1";
    g_boot.test_request_len = (uint16_t)strlen(selector);
    memcpy(g_boot.test_request, selector, g_boot.test_request_len);
    verify("BW", 0, "disabled", "ready", "disabled", 0); CHECK(!safe_run() && strstr(provenance, "fw_cfg=1"));
    reset(CBI_F_SAFE_MODE, false, false); verify("BW", 0, "disabled", "ready", "disabled", 0); CHECK(!safe_run()); /* no LFB/no storage */
    reset(CBI_F_SAFE_MODE, false, false); bios_result = input_result = -5; fail_expected = true;
    verify("BHN", 0, "disabled", "failed", "disabled", 1); CHECK(safe_run() == 1);
    CHECK(!safe_option("nosafe=1", 8) && !safe_option("safe=10", 7) && safe_option("safe=1", 6));
    /* A corrupt ordering/flag cannot pass the safe evidence. */
    reset(CBI_F_SAFE_MODE, false, true); verify("FBW", 0, "ready", "ready", "disabled", 0);
    activation_ledger[1].safe_flag_before = false;
    fail_expected = true;
    CHECK(!drivers_activation_ordered() && safe_run() == 1);
    activation_ledger[1].safe_flag_before = true;
    activation_ledger[1].seq = activation_ledger[0].seq;
    CHECK(!drivers_activation_ordered());
    activation_ledger[1].seq = 3;
    activation_ledger[1].tick = activation_ledger[0].tick - 1;
    CHECK(!drivers_activation_ordered());
    /* Even overflow is bounded: no overwrite beyond the 32-entry ledger. */
    activation_count = ACTIVATION_LEDGER_MAX - 1;
    logs = 0;
    struct activation_entry tail = { .seq = 33, .device = "fixture", .result = "ready", .reason = "bound" };
    activation_add(tail);
    CHECK(drivers_activation_count() == ACTIVATION_LEDGER_MAX && drivers_activation_get(31)->seq == 33);
    tail.seq = 34;
    activation_add(tail);
    CHECK(drivers_activation_count() == ACTIVATION_LEDGER_MAX && drivers_activation_get(31)->seq == 33 &&
          !drivers_activation_get(ACTIVATION_LEDGER_MAX));
    printf("activation records: max=%u bytes (limit 240); boot BEGIN/ledger/summary/existing records: PASS\n", max_activation_length);
    printf("runtime init/safe: %s (%u failures, %u records; ordering, flags, gating, fallbacks, failures)\n",
           failures ? "FAIL" : "PASS", failures, records);
    return failures ? 1 : 0;
}

#endif
