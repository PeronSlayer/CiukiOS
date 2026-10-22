/* f5-15C `installers` tier: the private checkpoint page between the
 * desktop's installer driver (apps/desktop/installer_title.c) and the kernel
 * observer (src/kernel/probes/installer_title_probe.inc).
 *
 * The kernel maps TITLE_RESULT_VA (TITLE_RESULT_PAGES pages; the lead's
 * reservation for this tier, below the f5-21b fixture pages' 64 KiB stride)
 * into the desktop before GATE_W32_FIXTURE "title-installer:<run><cd>".
 * The desktop writes the page, then reports `case=w32-title phase=<p>
 * serial=<n>` through the probe-report hook; the kernel reads the page in
 * that synchronous call and publishes records only from facts it verifies
 * itself (process identity and exit status, file inventory). The window
 * titles, registry key names and uninstall command are desktop-observed
 * (w32srv window directory and registry), never installer output.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TITLE_CONTROLLER_H
#define CIUKI_TITLE_CONTROLLER_H
#include <stdint.h>
#define TITLE_RESULT_VA 0x10100000u
#define TITLE_RESULT_PAGES 4u
#define TITLE_MAGIC 0x31544954u /* "TIT1" */
#define TITLE_FIXTURE_PREFIX "title-installer:"
#define TITLE_PROGRAM_FILE "CIUKI-OP.TXT"
#define TITLE_IDENTITY_FILE "CIUKI-ID.TXT"
enum title_phase {
    TITLE_READY = 1,      /* program parsed, w32srv answering; snapshot now */
    TITLE_STARTED,        /* segment's root process spawned (root_pid) */
    TITLE_UI,             /* one operator action about to be applied (ui) */
    TITLE_ROOT_EXITED,    /* root is a zombie; the kernel reads its status */
    TITLE_INSTALLED,      /* install tree gone; registry facts in the page */
    TITLE_UNINSTALLED,    /* uninstall tree gone; registry facts in the page */
    TITLE_DONE,           /* driver finished; failure==0 means success */
    TITLE_PHASES
};
enum title_failure {
    TITLE_OK, TITLE_F_PROGRAM, TITLE_F_SERVICE, TITLE_F_SPAWN, TITLE_F_STEP_TIMEOUT,
    TITLE_F_TIMEOUT, TITLE_F_CRASH, TITLE_F_INPUT, TITLE_F_REGISTRY, TITLE_F_UNINSTALL_SPAWN,
    TITLE_F_EXPECT, TITLE_F_RESTART, TITLE_F_STEPS_LEFT, TITLE_F_FAILURES
};
static const char *const title_failure_names[TITLE_F_FAILURES] = {
    "ok", "operator_program", "w32srv", "installer_spawn", "step_timeout", "segment_timeout",
    "installer_crash", "input", "registry", "uninstaller_spawn", "expect", "restart", "steps_left"};
enum title_action_kind {
    TITLE_A_STEP = 1, TITLE_A_DEFAULT, TITLE_A_RESTART, TITLE_A_CRASH, TITLE_A_GONE, TITLE_A_PROCESS
};
static const char *const title_action_names[] = {
    "", "step", "default", "restart", "crash", "gone", "process"};
#define TITLE_KEYS 4u
#define TITLE_KEY_BYTES 128u
struct title_result {
    uint32_t magic, version, phase, serial, segment; /* segment: 0 install, 1 uninstall */
    uint32_t failure, failed_step;
    uint32_t root_pid, root_exit;
    /* TITLE_UI: one record per applied action */
    uint32_t ui_step, ui_kind, ui_owner, ui_hwnd, ui_shot, ui_detail;
    char ui_title[160], ui_class[64];
    /* totals */
    uint32_t steps_done, defaults_used, restarts, crashes, processes;
    /* registry facts (desktop-observed through w32srv, read-only queries) */
    uint32_t uninstall_keys_before, uninstall_keys_after, new_keys, key_removed;
    char keys[TITLE_KEYS][TITLE_KEY_BYTES];
    char uninstall_command[520];
    uint32_t choices_default, uninstall_planned;
};
_Static_assert(sizeof(struct title_result) <= TITLE_RESULT_PAGES * 4096u, "title result page");
static inline int title_valid(const struct title_result *r)
{
    return r->magic == TITLE_MAGIC && r->version == 1 && r->phase && r->phase < TITLE_PHASES &&
        r->segment <= 1 && r->failure < TITLE_F_FAILURES && r->new_keys <= TITLE_KEYS &&
        r->ui_kind < sizeof(title_action_names) / sizeof(title_action_names[0]);
}

/* ---- kernel inventory policy (host-testable) -------------------------- */
/* Files the system itself rewrites during any run, never installer output:
 * the w32srv hive slots and journal (apps/w32srv/storage.c, restart.c) and
 * the boot logs. TEMP is counted separately (installer scratch, residue). */
static inline int title_upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static inline int title_prefix(const char *path, const char *prefix)
{
    for (; *prefix; prefix++, path++)
        if (title_upper((unsigned char)*path) != title_upper((unsigned char)*prefix)) return 0;
    return 1;
}
enum { TITLE_PATH_ORDINARY, TITLE_PATH_EXCLUDED, TITLE_PATH_TEMP, TITLE_PATH_SYSTEM };
static inline int title_path_class(const char *path)
{
    static const char *const excluded[] = {
        "C:\\WINDOWS\\SYSTEM.CRG.", "C:\\WINDOWS\\USER.CRG.", "C:\\WINDOWS\\CIUKIRST.LOG",
        "C:\\SYSTEM\\LOGS\\", "C:\\SYSTEM\\BOOTLOG", "C:\\PAGEFILE.SYS"};
    for (unsigned i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++)
        if (title_prefix(path, excluded[i])) return TITLE_PATH_EXCLUDED;
    if (title_prefix(path, "C:\\WINDOWS\\TEMP\\")) return TITLE_PATH_TEMP;
    if (title_prefix(path, "C:\\SYSTEM\\") || (title_prefix(path, "C:\\SYSTEM") && !path[9])) return TITLE_PATH_SYSTEM;
    return TITLE_PATH_ORDINARY;
}
static inline int title_menu_link(const char *path)
{
    unsigned n = 0;
    while (path[n]) n++;
    return title_prefix(path, "C:\\WINDOWS\\Start Menu\\Programs\\") && n > 4 &&
        title_prefix(path + n - 4, ".LNK");
}
static inline int title_link(const char *path)
{
    unsigned n = 0;
    while (path[n]) n++;
    return n > 4 && title_prefix(path + n - 4, ".LNK") &&
        (title_prefix(path, "C:\\WINDOWS\\Start Menu\\") || title_prefix(path, "C:\\WINDOWS\\Desktop\\"));
}
/* 64-bit FNV-1a of the upper-cased path: the snapshot key. */
static inline uint64_t title_path_hash(const char *path)
{
    uint64_t h = 14695981039346656037ull;
    for (; *path; path++) {
        h ^= (uint8_t)title_upper((unsigned char)*path);
        h *= 1099511628211ull;
    }
    return h;
}
struct title_entry { uint64_t hash; uint32_t size, stamp, first, attr; };
/* Snapshot entries are sorted by hash; a binary search names a path. */
static inline int title_find(const struct title_entry *e, unsigned count, uint64_t hash)
{
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (e[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && e[lo].hash == hash ? (int)lo : -1;
}
static inline void title_sort(struct title_entry *e, unsigned count)
{
    /* Shell sort: bounded stack, no allocation (kernel and host). */
    for (unsigned gap = count / 2; gap; gap /= 2)
        for (unsigned i = gap; i < count; i++) {
            struct title_entry t = e[i];
            unsigned j = i;
            while (j >= gap && e[j - gap].hash > t.hash) { e[j] = e[j - gap]; j -= gap; }
            e[j] = t;
        }
}
/* CiukiOS shell link, version 3 (apps/desktop/shell_link.h, the f5-15A
 * writer): "CIUKLNK3", version 1, five little-endian length fields at 16,
 * then name, target, arguments, directory, icon; returns the target. */
static inline int title_link_target(const uint8_t *b, unsigned n, const uint8_t **target, unsigned *length)
{
    if (n < 40 || memcmp(b, "CIUKLNK3", 8) || b[8] != 1) return 0;
    unsigned at = 40;
    for (unsigned i = 0; i < 5; i++) {
        unsigned p = 16 + i * 4;
        uint32_t len = (uint32_t)b[p] | (uint32_t)b[p + 1] << 8 | (uint32_t)b[p + 2] << 16 | (uint32_t)b[p + 3] << 24;
        if (len > n - at) return 0;
        if (i == 1) { *target = b + at; *length = len; }
        at += len;
    }
    return at == n;
}
#endif
