/* H14 (sec-user-06): the sectest model (apps/systools/sectest_model.c) on
 * fake operations, and the self-test's KDF (src/kernel/lib/pbkdf2.c)
 * against the account service's acct_derive (apps/accounts/crypto.c).
 *   sectest_test model   one block of records per scenario on stdout
 *   sectest_test kdf     hex digests: pbkdf2_hmac_sha256, acct_derive
 * SPDX-License-Identifier: MIT */
#include "sectest_model.h"
#include "accounts.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ciuki/pbkdf2.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ---- fake machine ------------------------------------------------------ */
static int role_uid, role_gid, groups[8], group_count, identity_error, env_result = 0;
static int results[ST_CHECKS];
static unsigned calls[ST_CHECKS], saver_calls, lines;
static char last_folder[32], last_foreign[32];
static int last_keep;
static void set_role(int role)
{
    memset(results, 0, sizeof(results)); memset(calls, 0, sizeof(calls));
    identity_error = 0; env_result = 0; group_count = 1;
    if (role == ST_ADMIN) { role_uid = 1000; role_gid = 545; groups[0] = 544; groups[1] = 545; group_count = 2; }
    else if (role == ST_STANDARD) { role_uid = 1001; role_gid = 545; groups[0] = 545; }
    else if (role == ST_GUEST) { role_uid = 501; role_gid = 546; groups[0] = 546; }
    else if (role == ST_SYSTEM) { role_uid = 0; role_gid = 0; groups[0] = 0; }
    else { role_uid = 2000; role_gid = 7; groups[0] = 7; }
    /* The production policy: what each role's real calls return. */
    for (int c = 0; c < ST_CHECKS; c++) {
        enum st_expect e = st_expect(role, (enum st_check)c);
        results[c] = e == ST_DENY ? -EACCES : 0;
    }
}
static int identity(void *ctx, struct st_identity *out)
{
    (void)ctx;
    if (identity_error) return identity_error;
    out->uid = (uint32_t)role_uid; out->gid = (uint32_t)role_gid; out->session = 3; out->count = (uint32_t)group_count;
    for (int i = 0; i < group_count; i++) out->groups[i] = (uint32_t)groups[i];
    return 0;
}
#define OP(name, id) static int name(void *ctx) { (void)ctx; calls[id]++; return results[id]; }
OP(own_profile, ST_OWN_PROFILE)
OP(other_profile, ST_OTHER_PROFILE)
OP(programs, ST_PROGRAMS)
OP(installer_store, ST_INSTALLER_STORE)
OP(installer_shortcuts, ST_INSTALLER_SHORTCUTS)
OP(machine_registry, ST_MACHINE_REGISTRY)
OP(machine_setting, ST_MACHINE_SETTING)
OP(raw_disk, ST_RAW_DISK)
OP(account_store, ST_ACCOUNT_STORE)
OP(backups, ST_BACKUPS)
static int environment(void *ctx, char *leaf, size_t size)
{
    (void)ctx; calls[ST_ENVIRONMENT]++;
    snprintf(leaf, size, env_result == -ENOENT ? "-" : "u%08x", (unsigned)role_uid);
    return env_result ? env_result : results[ST_ENVIRONMENT];
}
static int root_folder(void *ctx, const char *folder, int keep)
{ (void)ctx; calls[ST_ROOT_FOLDER]++; snprintf(last_folder, sizeof(last_folder), "%s", folder); last_keep = keep; return results[ST_ROOT_FOLDER]; }
static int foreign_folder(void *ctx, const char *folder)
{ (void)ctx; calls[ST_FOREIGN_FOLDER]++; snprintf(last_foreign, sizeof(last_foreign), "%s", folder); return results[ST_FOREIGN_FOLDER]; }
static int saver(void *ctx) { (void)ctx; saver_calls++; return 0; }
static void line(void *ctx, const char *text, size_t n)
{
    (void)ctx; lines++;
    CHECK(n > 0 && n < ST_LINE_MAX && strlen(text) == n && !memchr(text, '\n', n));
    printf("%.*s\n", (int)n, text);
}
static const struct st_ops ops = { NULL, identity, environment, own_profile, other_profile, programs, root_folder,
                                   foreign_folder, installer_store, installer_shortcuts, machine_registry,
                                   machine_setting, raw_disk, account_store, backups, saver, line };

static int run(const char *title, int argc, char **argv, struct st_summary *s)
{
    struct st_options o; int saver_only = 0;
    printf("== %s\n", title);
    if (st_options_parse(argc, argv, &o, &saver_only)) { printf("usage refused\n"); return 2; }
    return saver_only ? st_saver(&ops) : st_run(&ops, &o, s);
}

static int model(void)
{
    struct st_summary s;
    char *admin[] = { "--expect=admin", "--foreign=GAME", "--folder=GAME2" };
    char *standard[] = { "--expect=standard", "--keep" };
    char *guest[] = { "--expect=guest", "--foreign=GAME" };
    set_role(ST_ADMIN);
    CHECK(run("admin", 3, admin, &s) == 0 && s.role == ST_ADMIN && !s.failed && s.skipped == 1);
    CHECK(!calls[ST_MACHINE_SETTING] && calls[ST_FOREIGN_FOLDER] == 1 && !strcmp(last_folder, "GAME2") && !strcmp(last_foreign, "GAME"));
    set_role(ST_STANDARD);
    CHECK(run("standard", 2, standard, &s) == 0 && s.role == ST_STANDARD && !s.failed && !s.skipped);
    CHECK(!calls[ST_FOREIGN_FOLDER] && last_keep == 1 && !strcmp(last_folder, "GAME"));
    set_role(ST_GUEST);
    CHECK(run("guest", 2, guest, &s) == 0 && s.role == ST_GUEST && !s.failed);
    CHECK(!strcmp(last_folder, "SECTEST"));          /* Guest's default folder is never GAME */
    /* A refusal where the policy allows, an allowance where it refuses, an
     * unexpected error and an absent subsystem. */
    set_role(ST_STANDARD);
    results[ST_ROOT_FOLDER] = -EACCES; results[ST_PROGRAMS] = 0; results[ST_RAW_DISK] = -EIO; results[ST_BACKUPS] = -ENOENT;
    CHECK(run("standard wrong", 1, standard, &s) == 1 && s.failed == 3 && s.skipped == 1);
    /* Wrong role, SYSTEM in a session, unknown groups, identity failure: nothing attempted. */
    set_role(ST_STANDARD);
    char *expect_guest[] = { "--expect=guest" };
    CHECK(run("expect mismatch", 1, expect_guest, &s) == 1 && s.checks == 1 && !calls[ST_PROGRAMS]);
    set_role(ST_SYSTEM);
    CHECK(run("system", 0, NULL, &s) == 1 && s.system && s.checks == 1 && !calls[ST_OWN_PROFILE]);
    set_role(ST_UNKNOWN);
    CHECK(run("unknown", 0, NULL, &s) == 1 && s.checks == 1);
    set_role(ST_STANDARD); identity_error = -ENOSYS;
    CHECK(run("identity error", 0, NULL, &s) == 1 && s.checks == 1);
    /* No profile session: the environment check fails, the leaf is "-". */
    set_role(ST_STANDARD); env_result = -ENOENT;
    CHECK(run("no profile", 1, standard, &s) == 1);
    /* One selected check; the saver request; options refused. */
    set_role(ST_GUEST);
    char *one[] = { "--expect=guest", "--check=machine-setting" };
    CHECK(run("one check", 2, one, &s) == 0 && s.checks == 2 && calls[ST_MACHINE_SETTING] == 1 && !calls[ST_PROGRAMS]);
    char *save[] = { "saver" };
    CHECK(run("saver", 1, save, &s) == 0 && saver_calls == 1);
    char *bad[][1] = { { "--expect=system" }, { "--folder=../x" }, { "--folder=" }, { "--check=everything" },
                       { "--foreign=A/B" }, { "unknown" }, { "--folder=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456" } };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(run("bad option", 1, bad[i], &s) == 2);
    /* The expectation table itself (policy, tests compare it with the sources). */
    printf("== table\n");
    for (int c = 0; c < ST_CHECKS; c++) {
        printf("table %s", st_check_name((enum st_check)c));
        for (int r = ST_ADMIN; r <= ST_GUEST; r++) {
            enum st_expect e = st_expect(r, (enum st_check)c);
            printf(" %s=%s", st_role_name(r), e == ST_ALLOW ? "allow" : e == ST_DENY ? "deny" : "none");
        }
        printf("\n");
    }
    CHECK(st_denied(-EACCES) && st_denied(-EPERM) && st_denied(-EROFS) && !st_denied(0) && !st_denied(-EIO));
    printf("sectest model: %u records, 0 failures\n", lines);
    return 0;
}

static void hex(const uint8_t *b) { for (int i = 0; i < 32; i++) printf("%02x", b[i]); }
static int kdf(void)
{
    static const struct { const char *password; uint8_t salt[16]; } vectors[] = {
        { "selftest-password", { 0x43, 0x69, 0x75, 0x6b, 0x69, 0x4f, 0x53, 0x2d, 0x73, 0x65, 0x6c, 0x66, 0x74, 0x65, 0x73, 0x74 } },
        { "", { 0 } },
        { "Bob-pass-2026", { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } },
        { "0123456789012345678901234567890123456789012345678901234567890ab", { 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
                                                                              0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 } },
    };
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t a[32], b[32];
        CHECK(!pbkdf2_hmac_sha256(vectors[i].password, strlen(vectors[i].password),
                                  vectors[i].salt, sizeof(vectors[i].salt), ACCT_ITERATIONS, a));
        CHECK(!acct_derive(vectors[i].password, vectors[i].salt, ACCT_ITERATIONS, b));
        printf("kdf %u ", i); hex(a); printf(" "); hex(b); printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "model")) return model();
    if (argc == 2 && !strcmp(argv[1], "kdf")) return kdf();
    fprintf(stderr, "usage: sectest_test model|kdf\n");
    return 2;
}
