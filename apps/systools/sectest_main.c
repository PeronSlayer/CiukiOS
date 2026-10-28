/* sec-user-06: /Programs/sectest, the native binding of sectest_model.h.
 * Usage (from Start > Run, as the account under test):
 *   sectest [all] [--expect=admin|standard|guest] [--folder=NAME] [--foreign=NAME] [--keep] [--check=NAME]...
 *   sectest saver
 * Records go to the console, so the kernel copies each one to the serial
 * port and the boot log as "[console] sectest ...". Exit status 0 when every
 * attempted check met its expectation.
 * SPDX-License-Identifier: MIT */
#include "sectest_model.h"
#include <ciuki/abi.h>
#include <ciuki/recovery.h>
#include <ciuki/registry.h>
#include <ciuki/runtime.h>
#include <ciuki/security.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_errno(void) { return -(errno ? errno : EIO); }

static int identity(void *ctx, struct st_identity *out)
{
    (void)ctx;
    struct ciuki_credential c;
    memset(&c, 0, sizeof(c));
    if (ciuki_security_self(&c)) return fail_errno();
    out->uid = c.uid; out->gid = c.primary_gid; out->session = c.session_id;
    out->count = c.group_count > ST_GROUPS_MAX ? ST_GROUPS_MAX : c.group_count;
    for (uint32_t i = 0; i < out->count; i++) out->groups[i] = c.groups[i];
    return 0;
}
/* Create, write, close and remove one file; the first failure wins. */
static int scratch_file(const char *path)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return fail_errno();
    static const char text[] = "sectest\n";
    int e = write(fd, text, sizeof(text) - 1) == (ssize_t)(sizeof(text) - 1) ? 0 : fail_errno();
    if (close(fd) && !e) e = fail_errno();
    if (unlink(path) && !e) e = fail_errno();
    return e;
}
/* USERPROFILE is the kernel launch's committed root (/Windows/Profiles/
 * u%08x, sec-user-05); TMPDIR and HOME must lie inside it. */
static int inside(const char *path, const char *root)
{
    size_t n = strlen(root);
    return path && !strncmp(path, root, n) && path[n] == '/' && !strstr(path, "/..");
}
static const char *profile_root(void)
{
    const char *p = getenv("USERPROFILE");
    if (!p || !*p) p = getenv("CIUKI_PROFILE");
    return p && *p ? p : NULL;
}
static int environment(void *ctx, char *leaf, size_t size)
{
    (void)ctx;
    const char *root = getenv("USERPROFILE");
    if (!root || strncmp(root, "/Windows/Profiles/", 18) || !root[18] || strchr(root + 18, '/')) {
        snprintf(leaf, size, "-");
        return -ENOENT;
    }
    snprintf(leaf, size, "%s", root + 18);
    return inside(getenv("TMPDIR"), root) && inside(getenv("HOME"), root) ? 0 : -EINVAL;
}
/* A file in the account's private TEMP (TMPDIR inside the profile, sec-user-05),
 * created and removed, never the anchored profile root itself. */
static int own_profile(void *ctx)
{
    (void)ctx;
    const char *root = profile_root(), *temp = getenv("TMPDIR");
    if (!root) return -ENOENT;
    if (!inside(temp, root)) return -EINVAL;
    char path[200];
    if (snprintf(path, sizeof(path), "%s/SECTEST.TMP", temp) >= (int)sizeof(path)) return -ENAMETOOLONG;
    return scratch_file(path);
}
/* Another account's private root under /Windows/Profiles (K-Q1 u%08x names);
 * the caller's own root is recognised by its path, not by a uid spelling. */
static int other_profile(void *ctx)
{
    (void)ctx;
    const char *mine = profile_root();
    DIR *d = opendir("/Windows/Profiles");
    if (!d) return fail_errno();
    char path[96]; int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        const char *n = e->d_name;
        if ((n[0] != 'u' && n[0] != 'U') || strlen(n) != 9) continue;
        int hex = 1;
        for (unsigned i = 1; i < 9; i++) hex &= (n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f') || (n[i] >= 'A' && n[i] <= 'F');
        if (!hex) continue;
        snprintf(path, sizeof(path), "/Windows/Profiles/%s", n);
        if (mine && !strcasecmp(path, mine)) continue;
        found = 1;
    }
    closedir(d);
    if (!found) return -ENOENT;
    DIR *other = opendir(path);
    if (!other) return fail_errno();
    errno = 0;
    (void)readdir(other);
    int e = errno ? fail_errno() : 0;
    closedir(other);
    return e;
}
static int programs(void *ctx) { (void)ctx; return scratch_file("/Programs/SECTEST.TMP"); }
static int root_folder(void *ctx, const char *folder, int keep)
{
    (void)ctx;
    char dir[48], file[64];
    snprintf(dir, sizeof(dir), "/%s", folder);
    snprintf(file, sizeof(file), "/%s/SECTEST.TXT", folder);
    if (mkdir(dir, 0755)) return fail_errno();
    int e = scratch_file(file);
    if (!keep && rmdir(dir) && !e) e = fail_errno();
    return e;
}
static int foreign_folder(void *ctx, const char *folder)
{
    (void)ctx;
    char dir[48], file[64]; struct stat st;
    snprintf(dir, sizeof(dir), "/%s", folder);
    if (stat(dir, &st)) return fail_errno();
    snprintf(file, sizeof(file), "/%s/SECTEST.%u", folder, (unsigned)getuid());
    return scratch_file(file);
}
/* A planted DLL beside the store's installer, and a file in the protected
 * presentation folder (scripts/q3a_demo.py STORE_FOLDER, DESKTOP_FOLDER). */
static int installer_store(void *ctx)
{
    (void)ctx; struct stat st;
    if (stat("/Programs/Installers/Quake III Arena Demo", &st)) return fail_errno();
    return scratch_file("/Programs/Installers/Quake III Arena Demo/SECTEST.DLL");
}
static int installer_shortcuts(void *ctx)
{
    (void)ctx; struct stat st;
    if (stat("/Documents/Desktop/Quake III Arena Demo", &st)) return fail_errno();
    return scratch_file("/Documents/Desktop/Quake III Arena Demo/SECTEST.DLL");
}
static int machine_registry(void *ctx)
{
    (void)ctx;
    struct reg_root *r = reg_open_root("System");
    if (!r) return fail_errno();
    int e = reg_create_key(r, "System\\SecTest") ? fail_errno() : 0;
    if (!e && reg_set_string(r, "System\\SecTest", "Probe", "sectest")) e = fail_errno();
    if (!e && reg_delete(r, "System\\SecTest", NULL)) e = fail_errno();
    reg_close_root(r);
    return e;
}
/* The administrator check precedes any validation (syscalls_sysvc.c), so a
 * one-byte text proves the refusal without a valid performance record. */
static int machine_setting(void *ctx) { (void)ctx; return ciuki_system_setting_set(CIUKI_SYSTEM_SETTING_PERFORMANCE, "x", 1) ? fail_errno() : 0; }
static int raw_disk(void *ctx)
{
    (void)ctx;
    int fd = open("/dev/ata0", O_RDONLY);
    if (fd < 0) return errno == ENOENT ? -ENODEV : fail_errno();
    static unsigned char sector[512];
    ssize_t n = read(fd, sector, sizeof(sector));
    int e = n == (ssize_t)sizeof(sector) ? 0 : n < 0 ? fail_errno() : -EIO;
    close(fd);
    memset(sector, 0, sizeof(sector));
    return e;
}
static int open_listing(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return fail_errno();
    errno = 0;
    (void)readdir(d);
    int e = errno ? fail_errno() : 0;
    closedir(d);
    return e;
}
static int account_store(void *ctx) { (void)ctx; return open_listing("/System/Security"); }
static int backups(void *ctx) { (void)ctx; return open_listing("/System/SysBckup"); }
static int saver(void *ctx)
{
    (void)ctx;
    struct ciuki_screen_control q;
    memset(&q, 0, sizeof(q));
    q.version = CIUKI_SCREEN_VERSION; q.size = sizeof(q); q.action = 0xf140; q.value = -1;
    int32_t r = (int32_t)CU_CALL(DISPLAY, CIUKI_DISPLAY_SCREEN, CU_PTR(&q), 0, 0, 0, 0);
    return r < 0 ? r : 0;
}
/* One write per record: the console publishes each write as whole lines. */
static void line(void *ctx, const char *text, size_t n)
{
    (void)ctx;
    char out[ST_LINE_MAX + 1];
    if (n > ST_LINE_MAX) n = ST_LINE_MAX;
    memcpy(out, text, n); out[n] = '\n';
    (void)write(1, out, n + 1);
}

int main(int argc, char **argv)
{
    static const struct st_ops ops = { NULL, identity, environment, own_profile, other_profile, programs, root_folder,
                                       foreign_folder, installer_store, installer_shortcuts, machine_registry,
                                       machine_setting, raw_disk, account_store, backups, saver, line };
    struct st_options o;
    int saver_only = 0;
    if (st_options_parse(argc - 1, argv + 1, &o, &saver_only)) {
        static const char usage[] = "sectest usage: sectest [all] [--expect=admin|standard|guest] [--folder=NAME] [--foreign=NAME] [--keep] [--check=NAME] | saver";
        line(NULL, usage, sizeof(usage) - 1);
        return 2;
    }
    if (saver_only) return st_saver(&ops);
    struct st_summary s;
    return st_run(&ops, &o, &s);
}
