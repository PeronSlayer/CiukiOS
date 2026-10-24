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
static const char *desktop_reg = "; ciuki-registry generation=2\n[Desktop\\Theme]\nscheme = \"Ciuki\"\n";

static void tree(void)
{
    dir("/System"); dir("/System/Registry"); dir("/System/LOGS"); dir("/system"); dir("/system/settings");
    put("/System/Registry/System.reg", system_reg);
    put("/System/Registry/Desktop.reg", desktop_reg);
    put("/system/BOOT.CFG", "safe=0 serial=1\n");
    put("/system/settings/desktop.cfg", "wallpaper=Ciuk1\n");
    put("/system/settings/sounds.cfg", "scheme=default\n");
    put("/system/settings/notes.txt", "not configuration\n");
}

static int root_value(const char *key, const char *name, char *out, size_t cap)
{
    char d[600]; path_of(d, "/System/Registry");
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
        CHECK(made.files == 5 && made.valid);
    }
    int n = rc_list(root, list, 16);
    CHECK(n == RC_MAX_BACKUPS);
    for (int i = 0; i < n; i++) CHECK(list[i].valid && list[i].files == 5);
    CHECK(!strcmp(list[0].comment, "backup 5") && !strcmp(list[4].comment, "backup 1"));
    CHECK(list[0].created == 1500000 && !list[0].started && list[1].started);
    for (int i = 1; i < n; i++) CHECK(list[i - 1].created > list[i].created);
    /* The oldest (backup 0, RB000) was deleted and its number reused. */
    CHECK(!strcmp(list[0].name, "RB000.CRB"));
    CHECK(!exists("/System/SysBckup/RB005.CRB"));
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
    put("/System/Registry/System.reg", "[System\\Version\nbroken");
    CHECK(rc_scan(root, &damaged) == 0 && damaged == 1);
    unsigned restored; char from[16];
    CHECK(rc_autorestore(root, &restored, from) == 0 && restored == 1 && !strncmp(from, "RB", 2));
    CHECK(rc_scan(root, &damaged) == 0 && damaged == 0);
    char value[64];
    CHECK(root_value("System\\Version", "name", value, sizeof(value)) == 0 && !strcmp(value, "CiukiOS"));
    CHECK(exists("/System/Registry/System.reg.bad"));
    CHECK(!exists("/System/Registry/System.reg.txn") && !exists("/System/Registry/System.reg.new") &&
          !exists("/System/Registry/System.reg.bak"));
    CHECK(strstr(get("/System/Registry/Desktop.reg"), "scheme") != NULL);     /* untouched */
    /* Nothing damaged: nothing restored. */
    CHECK(rc_autorestore(root, &restored, from) == 0 && restored == 0 && !from[0]);
}

static void test_restore_everything(void)
{
    struct rc_backup made;
    CHECK(rc_backup(root, "before changes", 2000000000, true, &made) == 0);
    put("/system/BOOT.CFG", "safe=1\n");
    put("/system/settings/desktop.cfg", "wallpaper=none\n");
    put("/System/Registry/System.reg",
        "; ciuki-registry generation=40\n[System\\Version]\nname = \"Changed\"\n");
    CHECK(rc_restore(root, made.name) == 0);
    CHECK(!strcmp(get("/system/BOOT.CFG"), "safe=0 serial=1\n"));
    CHECK(!strcmp(get("/system/settings/desktop.cfg"), "wallpaper=Ciuk1\n"));
    char value[64];
    CHECK(root_value("System\\Version", "name", value, sizeof(value)) == 0 && !strcmp(value, "CiukiOS"));
    /* The restored root's generation goes past the replaced one (40). */
    char d[600]; path_of(d, "/System/Registry");
    struct reg_root *r = reg_open_root_at("System", d);
    CHECK(r && reg_generation(r) == 41);
    reg_close_root(r);
    CHECK(!exists("/System/Registry/System.reg.txn"));
    CHECK(!exists("/system/BOOT.CFG.rcn") && !exists("/system/BOOT.CFG.rco"));
    CHECK(rc_restore(root, "../x") == -EINVAL && rc_restore(root, "RB099.CRB") == -ENOENT);
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
    put("/System/Registry/Desktop.reg", "\x01\x02 not a registry");
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
    test_damaged_archives();
    test_system_file_checker();
    test_scandisk_text();
    test_bootcfg_and_msconfig();
    printf("sysrecovery: %u failures\n", failures);
    return failures != 0;
}
