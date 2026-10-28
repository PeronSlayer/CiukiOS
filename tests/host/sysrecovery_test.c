/* G14 Registry Checker, System File Checker and ScanDisk report text
 * (apps/sysrecovery) on a host fixture tree: argv[1] is an empty directory
 * standing for the system drive's root.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/* System headers first: abi.h then leaves the C library's macros alone. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../apps/sysrecovery/recovery.h"
#include "../../apps/accessories/msconfig.h"
#include <ciuki/registry.h>
#include <ciuki/sha256.h>

#include <ciuki/devices.h>
/* libreg's Hardware root is never opened here. */
int ciuki_devices(struct ciuki_devices_info *s) { (void)s; errno = ENOSYS; return -1; }
static unsigned failures;
static const char *root;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void path_of(char *out, const char *p) { snprintf(out, 600, "%s%s", root, p); }
static void put(const char *p, const char *text)
{
    char f[600]; path_of(f, p);
    FILE *o = fopen(f, "wb"); CHECK(o); if (!o) return;
    fputs(text, o); fclose(o);
}
static char *get(const char *p)
{
    char f[600]; path_of(f, p);
    FILE *i = fopen(f, "rb"); if (!i) return NULL;
    static char buf[65536]; size_t n = fread(buf, 1, sizeof(buf) - 1, i); buf[n] = 0; fclose(i);
    return buf;
}
static bool exists(const char *p) { char f[600]; struct stat st; path_of(f, p); return !stat(f, &st); }
static void dir(const char *p) { char f[600]; path_of(f, p); mkdir(f, 0755); }
static const char *system_reg =
    "; ciuki-registry generation=5\n[System\\Version]\nname = \"CiukiOS\"\nversion = \"1.0\"\n";
static const char *programs_reg = "; ciuki-registry generation=2\n[Programs\\Notepad]\nfont = \"Courier\"\n";
/* sec-sysvc2: the machine registry is the SYSTEM registry broker's (sec-k06):
 * its native System and Programs roots and the Win32 machine hive slots. */
#define MACHINE RC_MACHINE
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    c = ~c;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}
static void w32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* A committed Win32 machine hive slot (apps/w32srv/storage.c CRG1). */
static size_t slot_bytes(uint8_t *b, uint64_t sequence, const char *text)
{
    size_t n = strlen(text); memset(b, 0, 40);
    memcpy(b, "CRG1", 4); w32(b + 4, 1); w32(b + 12, (uint32_t)sequence); w32(b + 16, (uint32_t)(sequence >> 32));
    w32(b + 20, (uint32_t)n); memcpy(b + 40, text, n); w32(b + 24, crc(0, b + 40, n)); w32(b + 28, crc(0, b, 28));
    w32(b + 32, 0x43524731u);
    return 40 + n;
}
static void put_slot(const char *p, uint64_t sequence, const char *text)
{
    uint8_t b[256]; size_t n = slot_bytes(b, sequence, text);
    char f[600]; snprintf(f, sizeof(f), "%s%s", root, p);
    FILE *o = fopen(f, "wb"); CHECK(o); if (!o) return;
    CHECK(fwrite(b, 1, n, o) == n); fclose(o);
}
static bool slot_is(const char *p, uint64_t sequence, const char *text)
{
    uint8_t want[256], got[300]; size_t n = slot_bytes(want, sequence, text);
    char f[600]; snprintf(f, sizeof(f), "%s%s", root, p);
    FILE *i = fopen(f, "rb"); if (!i) return false;
    size_t k = fread(got, 1, sizeof(got), i); fclose(i);
    return k == n && !memcmp(got, want, n);
}

static void tree(void)
{
    dir("/System"); dir("/System/Security"); dir("/System/Security/Registry"); dir(MACHINE);
    dir("/System/LOGS"); dir("/system"); dir("/system/settings");
    put(MACHINE "/System.reg", system_reg);
    put(MACHINE "/Programs.reg", programs_reg);
    put_slot(MACHINE "/SYSTEM.CRG.0", 5, "[HKLM]\nslot zero\n");
    put("/system/BOOT.CFG", "safe=0 serial=1\n");
    put("/system/HWPROF.CFG", "0:name=Original Configuration\n");
    put("/system/settings/desktop.cfg", "wallpaper=Ciuk1\n");
    put("/system/settings/sounds.cfg", "scheme=default\n");
    put("/system/settings/notes.txt", "not configuration\n");
}

static int root_value(const char *key, const char *name, char *out, size_t cap)
{
    char d[600]; path_of(d, MACHINE);
    struct reg_root *r = reg_open_root_at("System", d);
    if (!r) return -1;
    size_t n = cap; int e = reg_get_string(r, key, name, out, &n);
    reg_close_root(r);
    return e;
}

static void test_backups_rotation_and_listing(void)
{
    struct rc_backup made, list[16];
    for (int i = 0; i < 6; i++) {
        char comment[32]; snprintf(comment, sizeof(comment), "backup %d", i);
        CHECK(rc_backup(root, comment, 1000000 + i * 100000, i % 2 == 0, &made) == 0);
        CHECK(made.files == 7 && made.valid);  /* 2 roots, 1 hive slot, BOOT.CFG, HWPROF.CFG, 2 .cfg */
    }
    int n = rc_list(root, list, 16);
    CHECK(n == RC_MAX_BACKUPS);
    for (int i = 0; i < n; i++) CHECK(list[i].valid && list[i].files == 7);
    CHECK(!strcmp(list[0].comment, "backup 5") && !strcmp(list[4].comment, "backup 1"));
    CHECK(list[0].created == 1500000 && !list[0].started && list[1].started);
    for (int i = 1; i < n; i++) CHECK(list[i - 1].created > list[i].created);
    /* The oldest (backup 0, RB000) was deleted and its number reused. */
    CHECK(!strcmp(list[0].name, "RB000.CRB"));
    CHECK(!exists("/System/SysBckup/RB005.CRB"));
    /* The broker's wire record round-trips. */
    uint8_t wire[RC_WIRE]; struct rc_backup back;
    rc_wire_put(&list[1], wire); rc_wire_get(&back, wire);
    CHECK(!strcmp(back.name, list[1].name) && back.created == list[1].created && back.files == 7 &&
          back.started == list[1].started && back.valid && !strcmp(back.comment, list[1].comment));
}

static void test_daily(void)
{
    int made;
    /* Newest valid started backup is "backup 4" at 1400000; 1500000 is not started. */
    CHECK(rc_daily(root, 1400000 + 3600, 0, &made) == 0 && made == 0);
    /* 1500000 (backup 5) is the same day but not a completed start. */
    CHECK(rc_daily(root, 1500000 + 60, 0, &made) == 0 && made == 1);
    CHECK(rc_daily(root, 1500000 + 120, 0, &made) == 0 && made == 0);
    struct rc_backup list[8];
    CHECK(rc_list(root, list, 8) == RC_MAX_BACKUPS && !strcmp(list[0].comment, "Daily backup") && list[0].started);
    /* Days are local: at UTC+2, 1555190 (23:13 UTC) is already the next day. */
    CHECK(rc_daily(root, 1555190, 0, &made) == 0 && made == 0);
    CHECK(rc_daily(root, 1555190, 120, &made) == 0 && made == 1);
}

static void test_scan_and_autorestore(void)
{
    unsigned damaged = 99;
    CHECK(rc_scan(root, &damaged) == 0 && damaged == 0);
    CHECK(!strcmp(rc_root_name(0), "System") && !strcmp(rc_root_name(1), "Programs") && !rc_root_name(RC_ROOTS + 1));
    put(MACHINE "/System.reg", "[System\\Version\nbroken");
    CHECK(rc_scan(root, &damaged) == 0 && damaged == 1);
    unsigned restored; char from[16];
    CHECK(rc_autorestore(root, &restored, from) == 0 && restored == 1 && !strncmp(from, "RB", 2));
    CHECK(rc_scan(root, &damaged) == 0 && damaged == 0);
    char value[64];
    CHECK(root_value("System\\Version", "name", value, sizeof(value)) == 0 && !strcmp(value, "CiukiOS"));
    CHECK(exists(MACHINE "/System.reg.bad"));
    CHECK(!exists(MACHINE "/System.reg.txn") && !exists(MACHINE "/System.reg.new") && !exists(MACHINE "/System.reg.bak"));
    CHECK(strstr(get(MACHINE "/Programs.reg"), "Courier") != NULL);     /* untouched */
    /* Nothing damaged: nothing restored. */
    CHECK(rc_autorestore(root, &restored, from) == 0 && restored == 0 && !from[0]);
}

static void test_restore_everything(void)
{
    struct rc_backup made;
    /* Only the committed hive slot the loader would take (the valid one with
     * the higher sequence) is backed up. */
    put_slot(MACHINE "/SYSTEM.CRG.1", 6, "[HKLM]\nslot one\n");
    CHECK(rc_backup(root, "before changes", 2000000000, true, &made) == 0 && made.files == 7);
    put("/system/BOOT.CFG", "safe=1\n");
    put("/system/HWPROF.CFG", "0:name=Changed\n");
    put("/system/settings/desktop.cfg", "wallpaper=none\n");
    put(MACHINE "/System.reg", "; ciuki-registry generation=40\n[System\\Version]\nname = \"Changed\"\n");
    /* A newer machine hive slot than the backup's would win at the next load. */
    put_slot(MACHINE "/SYSTEM.CRG.0", 9, "[HKLM]\nnewer slot zero\n");
    put(MACHINE "/SYSTEM.CRG.1", "damaged");
    CHECK(rc_restore(root, made.name) == 0);
    CHECK(!strcmp(get("/system/BOOT.CFG"), "safe=0 serial=1\n"));
    CHECK(!strcmp(get("/system/HWPROF.CFG"), "0:name=Original Configuration\n"));
    CHECK(!strcmp(get("/system/settings/desktop.cfg"), "wallpaper=Ciuk1\n"));
    CHECK(slot_is(MACHINE "/SYSTEM.CRG.1", 6, "[HKLM]\nslot one\n") && !exists(MACHINE "/SYSTEM.CRG.0"));
    char value[64];
    CHECK(root_value("System\\Version", "name", value, sizeof(value)) == 0 && !strcmp(value, "CiukiOS"));
    /* The restored root's generation goes past the replaced one (40). */
    char d[600]; path_of(d, MACHINE);
    struct reg_root *r = reg_open_root_at("System", d);
    CHECK(r && reg_generation(r) == 41);
    reg_close_root(r);
    CHECK(!exists(MACHINE "/System.reg.txn"));
    CHECK(!exists("/system/BOOT.CFG.rcn") && !exists("/system/BOOT.CFG.rco"));
    CHECK(rc_restore(root, "../x") == -EINVAL && rc_restore(root, "RB099.CRB") == -ENOENT);
}

/* sec-sysvc2: the broker's next-start restore (rc_stage_restore at the
 * request, rc_apply_pending when the broker starts) and its status. */
static void test_staged_restore(void)
{
    struct rc_backup made; char name[16], applied[16];
    CHECK(rc_backup(root, "for the next start", 2050000000, true, &made) == 0);
    CHECK(rc_pending(root, name) == 0 && !name[0]);
    CHECK(rc_stage_restore(root, "../RB000.CRB") == -EINVAL && rc_stage_restore(root, "RB098.CRB") == -ENOENT);
    CHECK(rc_stage_restore(root, made.name) == 0 && rc_pending(root, name) == 1 && !strcmp(name, made.name));
    struct rc_status st;
    CHECK(rc_status(root, &st) == 0 && !st.damaged && !strcmp(st.pending, made.name));
    put("/system/settings/sounds.cfg", "scheme=changed\n");
    CHECK(!strcmp(get("/system/settings/sounds.cfg"), "scheme=changed\n"));   /* nothing restored yet */
    CHECK(rc_apply_pending(root, applied) == 0 && !strcmp(applied, made.name));
    CHECK(!strcmp(get("/system/settings/sounds.cfg"), "scheme=default\n") && rc_pending(root, name) == 0);
    CHECK(rc_apply_pending(root, applied) == 0 && !applied[0]);                /* once */
    /* A request whose backup vanished is dropped, not retried at every start. */
    CHECK(rc_stage_restore(root, made.name) == 0);
    char f[600]; path_of(f, "/System/SysBckup/"); strcat(f, made.name); CHECK(!unlink(f));
    CHECK(rc_apply_pending(root, applied) == -ENOENT && !applied[0] && rc_pending(root, name) == 0);
    put("/System/SysBckup/RESTORE.PND", "../../x\n");
    CHECK(rc_pending(root, name) == -EILSEQ && rc_apply_pending(root, applied) == -EILSEQ && !exists("/System/SysBckup/RESTORE.PND"));
}

/* A pre-sec-k06 backup (raw /System/Registry roots) still restores its
 * configuration files; the legacy roots nothing reads are skipped. */
static void test_legacy_archive(void)
{
    static const char *const paths[] = { "/System/Registry/System.reg", "/system/settings/desktop.cfg", "/etc/passwd" };
    static const char *const data[] = { "; ciuki-registry generation=1\n[System\\Old]\n", "wallpaper=Legacy\n", "x" };
    for (unsigned with_bad = 0; with_bad < 2; with_bad++) {
        unsigned count = with_bad ? 3 : 2; size_t total = 0;
        for (unsigned i = 0; i < count; i++) total += strlen(data[i]);
        size_t size = 64 + count * 104 + total + 4;
        uint8_t *b = calloc(1, size); CHECK(b); if (!b) return;
        memcpy(b, "CRB1", 4); w32(b + 4, 1); int64_t when = 1000; memcpy(b + 8, &when, 8); w32(b + 16, count); w32(b + 20, 1);
        uint8_t *payload = b + 64 + count * 104;
        for (unsigned i = 0; i < count; i++) {
            uint8_t *en = b + 64 + i * 104; size_t n = strlen(data[i]);
            memcpy(en, paths[i], strlen(paths[i]) + 1); w32(en + 96, (uint32_t)n); w32(en + 100, crc(0, (const uint8_t *)data[i], n));
            memcpy(payload, data[i], n); payload += n;
        }
        w32(b + size - 4, crc(0, b, size - 4));
        char f[600]; path_of(f, "/System/SysBckup/RB090.CRB");
        FILE *o = fopen(f, "wb"); CHECK(o); if (o) { fwrite(b, 1, size, o); fclose(o); }
        free(b);
        int e = rc_restore(root, "RB090.CRB");
        if (!with_bad) {
            CHECK(e == 0 && !strcmp(get("/system/settings/desktop.cfg"), "wallpaper=Legacy\n"));
            CHECK(!exists("/System/Registry/System.reg.rcn") && !strstr(get(MACHINE "/System.reg"), "Old"));
        } else CHECK(e == -EINVAL);                                  /* a path no backup collects */
        CHECK(!unlink(f));
    }
}

/* sec-sysvc2: the Win32 machine hive (no valid committed slot) is restored
 * only at the broker's start, before its loader runs; a repair while the
 * broker runs leaves it (the running broker rewrites a slot itself). */
static void test_hive_autorestore(void)
{
    struct rc_backup made; unsigned restored = 0; char from[16];
    put_slot(MACHINE "/SYSTEM.CRG.0", 11, "[HKLM]\nfresh\n");
    put(MACHINE "/SYSTEM.CRG.1", "damaged");
    CHECK(rc_backup(root, "hive", 2200000000, true, &made) == 0);
    put(MACHINE "/SYSTEM.CRG.0", "damaged too");
    CHECK(rc_autorestore(root, &restored, from) == 0 && !restored && !from[0]);
    CHECK(rc_autorestore_start(root, &restored, from) == 0 && restored == RC_HIVE && !strcmp(from, made.name));
    CHECK(slot_is(MACHINE "/SYSTEM.CRG.0", 11, "[HKLM]\nfresh\n") && !exists(MACHINE "/SYSTEM.CRG.1"));
    CHECK(exists(MACHINE "/SYSTEM.CRG.0.bad") && exists(MACHINE "/SYSTEM.CRG.1.bad") && !strcmp(rc_root_name(RC_ROOTS), "Win32 machine"));
    CHECK(rc_autorestore_start(root, &restored, from) == 0 && !restored);    /* nothing damaged now */
}

static void test_damaged_archives(void)
{
    struct rc_backup made, list[8];
    CHECK(rc_backup(root, "to damage", 2100000000, true, &made) == 0);
    char f[600]; path_of(f, "/System/SysBckup/"); strcat(f, made.name);
    int fd = open(f, O_RDWR); CHECK(fd >= 0);
    char c; CHECK(pread(fd, &c, 1, 200) == 1); c ^= 0x55; CHECK(pwrite(fd, &c, 1, 200) == 1); close(fd);
    int n = rc_list(root, list, 8);
    bool seen = false;
    for (int i = 0; i < n; i++) if (!strcmp(list[i].name, made.name)) { seen = true; CHECK(!list[i].valid); }
    CHECK(seen);
    CHECK(rc_restore(root, made.name) == -EILSEQ);
    /* Autorestore skips it and takes an older good copy. */
    put(MACHINE "/Programs.reg", "\x01\x02 not a registry");
    unsigned restored; char from[16];
    CHECK(rc_autorestore(root, &restored, from) == 0 && restored == 2 && strcmp(from, made.name));
}

static void sha_hex(const char *text, char hex[65])
{
    uint8_t d[32]; sha256(text, strlen(text), d); sha256_hex(d, hex);
}
static void test_system_file_checker(void)
{
    dir("/Programs"); dir("/src"); dir("/src/Programs");
    const char *files[3][2] = { { "/Programs/desktop", "desktop image" }, { "/Programs/files", "files image" },
                                { "/System/VMM.ELF", "kernel image" } };
    char list[2048] = "; CiukiOS System File Checker baseline: sha256 size path\n";
    for (int i = 0; i < 3; i++) {
        put(files[i][0], files[i][1]);
        char hex[65]; sha_hex(files[i][1], hex);
        char line[256]; snprintf(line, sizeof(line), "%s %zu %s\n", hex, strlen(files[i][1]), files[i][0]);
        strcat(list, line);
    }
    put(SFC_LIST, list);
    struct sfc_result r;
    CHECK(sfc_scan(root, &r) == 0 && r.checked == 3 && r.count == 0);
    sfc_free(&r);
    put("/Programs/files", "changed by an installer");
    char f[600]; path_of(f, "/Programs/desktop"); unlink(f);
    CHECK(sfc_scan(root, &r) == 0 && r.changed == 1 && r.deleted == 1 && r.count == 2);
    for (unsigned i = 0; i < r.count; i++)
        CHECK((r.problems[i].state == SFC_DELETED) == !strcmp(r.problems[i].path, "/Programs/desktop"));
    CHECK(sfc_log(root, &r, 1700000000) == 0);
    sfc_free(&r);
    char *log = get(SFC_LOG);
    CHECK(log && strstr(log, "3 checked, 1 changed, 1 deleted") && strstr(log, "deleted /Programs/desktop"));
    /* Restore: only the original source is accepted. */
    put("/src/Programs/desktop", "not the original");
    char source[600]; path_of(source, "/src");
    CHECK(sfc_restore(root, "/Programs/desktop", source) == -EILSEQ);
    put("/src/Programs/desktop", "desktop image");
    CHECK(sfc_restore(root, "/Programs/desktop", source) == 0);
    CHECK(!strcmp(get("/Programs/desktop"), "desktop image"));
    put("/src/Programs/files", "files image");
    CHECK(sfc_restore(root, "/Programs/files", source) == 0);
    CHECK(!strcmp(get("/Programs/files"), "files image"));
    CHECK(!strcmp(get(SFC_HELPDESK "/files"), "changed by an installer"));   /* backed up first */
    CHECK(sfc_restore(root, "/Programs/unknown", source) == -ENOENT);
    /* Update verification information: the new file becomes the baseline. */
    put("/System/VMM.ELF", "updated kernel");
    CHECK(sfc_scan(root, &r) == 0 && r.changed == 1); sfc_free(&r);
    CHECK(sfc_update(root, "/System/VMM.ELF") == 0);
    CHECK(sfc_scan(root, &r) == 0 && r.count == 0 && r.checked == 3); sfc_free(&r);
    CHECK(strstr(get(SFC_OVERRIDES), "/System/VMM.ELF") != NULL);
}

static void test_scandisk_text(void)
{
    struct ciuki_scandisk r; char out[2048];
    memset(&r, 0, sizeof(r));
    r.drive = 2; scandisk_text(&r, out, sizeof(out));
    CHECK(strstr(out, "has not checked drive C:"));
    r.flags = CIUKI_SCANDISK_RAN; r.cluster_bytes = 4096; r.clusters = 130000; r.free_clusters = 100000;
    r.files = 1234; r.directories = 56; r.file_clusters = 25000; r.directory_clusters = 60;
    scandisk_text(&r, out, sizeof(out));
    CHECK(strstr(out, "did not find any errors"));
    CHECK(strstr(out, "532,480,000 bytes total disk space"));
    CHECK(strstr(out, "409,600,000 bytes available on disk"));
    CHECK(strstr(out, "bytes in 1,234 user files") && strstr(out, "bytes in 57 folders"));
    CHECK(strstr(out, "4,096 bytes in each allocation unit"));
    r.flags |= CIUKI_SCANDISK_ERRORS | CIUKI_SCANDISK_FIX | CIUKI_SCANDISK_FIXED | CIUKI_SCANDISK_AT_START;
    r.lost_chains = 2; r.lost_clusters = 7; r.salvaged_files = 2; strcpy(r.found, "FOUND.000");
    r.crosslinked_files = 1; r.crosslinks_fixed = 1; strcpy(r.first_problem, "B.BIN");
    scandisk_text(&r, out, sizeof(out));
    CHECK(strstr(out, "because CiukiOS was not shut down properly") && strstr(out, "fixed them all"));
    CHECK(strstr(out, "2 lost file fragments (7 allocation units) saved in") && strstr(out, "2 files in \\FOUND.000"));
    CHECK(strstr(out, "1 cross-linked files, 1 copied") && strstr(out, "First damaged entry: B.BIN"));
    r.flags = CIUKI_SCANDISK_RAN | CIUKI_SCANDISK_ERRORS | CIUKI_SCANDISK_SCHEDULED;
    scandisk_text(&r, out, sizeof(out));
    CHECK(strstr(out, "fixed the next time CiukiOS starts"));
    r.result = -5; scandisk_text(&r, out, sizeof(out));
    CHECK(strstr(out, "could not check drive C: (error 5)"));
    size_t need = scandisk_text(&(struct ciuki_scandisk){ .flags = CIUKI_SCANDISK_RAN, .cluster_bytes = 512 }, out, 8);
    CHECK(need > 8 && strlen(out) == 7);                  /* truncated, terminated, full length returned */
    r.result = 0;
    CHECK(scandisk_log(root, &r, 1700000000) == 0 && strstr(get("/System/LOGS/SCANDISK.LOG"), "ScanDisk, "));
}

static void test_bootcfg_and_msconfig(void)
{
    struct bootcfg_options o; char out[BOOTCFG_MAX + 1]; size_t n;
    const char *in = "safe=0 serial=1\nprobe=f1:safe run=12345678\naccel=0 menu=1\n";
    bootcfg_read(in, strlen(in), &o);
    CHECK(o.menu == 1 && o.autoscan == 1 && o.serial == 1);
    o.menu = 0; o.autoscan = 0; o.serial = 0;
    CHECK(bootcfg_apply(in, strlen(in), &o, out, &n) == 0 && n == strlen(out));
    CHECK(!strcmp(out, "safe=0 probe=f1:safe run=12345678\naccel=0 serial=0 autoscan=0\n"));
    struct bootcfg_options back; bootcfg_read(out, n, &back);
    CHECK(!back.menu && back.autoscan == 0 && !back.serial);
    o.menu = 1; o.autoscan = 2; o.serial = 1;
    CHECK(bootcfg_apply("", 0, &o, out, &n) == 0 && !strcmp(out, "serial=1 menu=1 autoscan=2\n"));
    char big[BOOTCFG_MAX + 1]; memset(big, 'x', 120); big[120] = 0;
    CHECK(bootcfg_apply(big, 120, &o, out, &n) == -EFBIG);
    put("/system/BOOT.CFG", "safe=0 serial=1\n");
    o.menu = 1; o.autoscan = 1; o.serial = 1;
    CHECK(bootcfg_update(root, &o) == 0 && !strcmp(get("/system/BOOT.CFG"), "safe=0 serial=1 menu=1\n"));
    CHECK(!exists("/system/BOOT.CFG.new") && !exists("/system/BOOT.CFG.old"));
    /* System Configuration: status <-> startup selection. */
    struct ciuki_recovery_status s; memset(&s, 0, sizeof(s));
    struct msconfig_state st;
    msconfig_from_status(&s, "menu=1 autoscan=0", 17, &st);
    CHECK(st.mode == MC_NORMAL && st.enabled == CIUKI_RECOVERY_CLASS_MASK && st.advanced.menu && st.advanced.autoscan == 0);
    s.disabled = CIUKI_RECOVERY_SOUND | CIUKI_RECOVERY_USB; s.request = CIUKI_RECOVERY_REQ_SCANDISK;
    msconfig_from_status(&s, NULL, 0, &st);
    CHECK(st.mode == MC_SELECTIVE && st.enabled == (CIUKI_RECOVERY_CLASS_MASK & ~(CIUKI_RECOVERY_SOUND | CIUKI_RECOVERY_USB)));
    uint8_t disabled, request;
    msconfig_to_request(&st, s.request, &disabled, &request);
    CHECK(disabled == (CIUKI_RECOVERY_SOUND | CIUKI_RECOVERY_USB) && request == CIUKI_RECOVERY_REQ_SCANDISK);
    st.mode = MC_DIAGNOSTIC;
    msconfig_to_request(&st, CIUKI_RECOVERY_REQ_SCANDISK | CIUKI_RECOVERY_REQ_THOROUGH | CIUKI_RECOVERY_REQ_MENU, &disabled, &request);
    CHECK(!disabled && request == (CIUKI_RECOVERY_REQ_SCANDISK | CIUKI_RECOVERY_REQ_THOROUGH | CIUKI_RECOVERY_REQ_MENU));
    s.request = CIUKI_RECOVERY_REQ_MENU; msconfig_from_status(&s, NULL, 0, &st);
    CHECK(st.mode == MC_DIAGNOSTIC);
    st.mode = MC_NORMAL; msconfig_to_request(&st, CIUKI_RECOVERY_REQ_MENU, &disabled, &request);
    CHECK(!disabled && !request);
    CHECK(!strcmp(msconfig_class_label(1), "Load the sound drivers") && !msconfig_class_label(9)[0]);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    root = argv[1];
    tree();
    test_backups_rotation_and_listing();
    test_daily();
    test_scan_and_autorestore();
    test_restore_everything();
    test_staged_restore();
    test_legacy_archive();
    test_damaged_archives();
    test_hive_autorestore();
    test_system_file_checker();
    test_scandisk_text();
    test_bootcfg_and_msconfig();
    printf("sysrecovery: %u failures\n", failures);
    return failures != 0;
}
