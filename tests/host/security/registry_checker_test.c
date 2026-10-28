/* H07 sec-sysvc2: Registry Checker in the production SYSTEM registry broker
 * (apps/registry/broker.c, checker.c) driven through the production client
 * (apps/sysrecovery/regcheck.c, built with RC_REMOTE_TEST so root "" is a
 * broker request). argv[1] is an empty directory standing for C:. Peers:
 * an administrator, a standard user, Guest, the interim administrator session
 * (no committed profile) and SYSTEM. The broker's engine works on the real
 * files of the fixture; only the kernel binding (peer identities, the start
 * status and the clock) is a fake. SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../apps/registry/broker.h"
#include "../../../apps/registry/checker.h"
#include "../../../apps/sysrecovery/recovery.h"
#include "../../../sdk/libreg/internal.h"
#include "../../../sdk/libreg/transport.h"

static unsigned failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
static const char *root;

/* ---- the kernel binding ---- */
enum { ADMIN, STANDARD, GUEST, INTERIM, SYSTEM, PEERS };
static const char *const peer_name[PEERS] = { "administrator", "standard user", "Guest", "interim session", "SYSTEM" };
static struct rb_identity peers[PEERS];
static int live[PEERS] = { 1, 1, 1, 1, 1 };
static int role(void *p) { (void)p; return 0; }
static int inspect(void *p, uintptr_t lease, struct rb_identity *out)
{ (void)p; if (!lease || lease > PEERS || !live[lease - 1]) return -ESRCH; *out = peers[lease - 1]; return 0; }
static int retain(void *p, uintptr_t lease) { (void)p; return !lease || lease > PEERS || !live[lease - 1] ? -ESRCH : 0; }
static void release(void *p, uintptr_t lease) { (void)p; (void)lease; }
static int storage_read(void *p, unsigned root_id, char **out, size_t *n) { (void)p; (void)root_id; (void)out; (void)n; return -EACCES; }
static int storage_write(void *p, unsigned root_id, const char *v, size_t n, uint64_t g) { (void)p; (void)root_id; (void)v; (void)n; (void)g; return -EACCES; }
static struct rb_broker broker;
static unsigned slot[PEERS];
static int client = ADMIN;
/* The client's transport (sdk/libreg/transport.h) reaches the broker on the
 * slot of the current peer. */
void reg_remote_close(struct reg_remote *r) { r->fd = -1; }
int reg_registry_rpc(struct reg_remote *r, unsigned op, const void *in, size_t n, void *out, size_t cap, size_t *used)
{
    (void)r;
    int e = rb_exchange(&broker, slot[client], op, in, n, out, cap, used);
    return e ? reg_fail(-e) : 0;
}
struct rt *reg_hardware(uint64_t previous) { (void)previous; reg_fail(EROFS); return NULL; }
static int started = 1;
static int64_t clock_now = 1700000000; static int clock_offset;
static unsigned log_lines;
static int start_status(void *p) { (void)p; return started; }
static int64_t now(void *p, int *offset) { (void)p; *offset = clock_offset; return clock_now; }
static void log_line(void *p, const char *line) { (void)p; log_lines++; printf("%s\n", line); }
static struct rb_checker checker = { .started = start_status, .now = now, .log = log_line };

/* ---- the fixture drive ---- */
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
static int exists(const char *p) { char f[600]; struct stat st; path_of(f, p); return !stat(f, &st); }
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}
/* A committed Win32 machine hive slot (apps/w32srv/storage.c CRG1). */
static size_t slot_bytes(uint8_t *b, uint32_t sequence, const char *text)
{
    size_t n = strlen(text); memset(b, 0, 40);
    memcpy(b, "CRG1", 4); rb_put32(b + 4, 1); rb_put32(b + 12, sequence); rb_put32(b + 20, (uint32_t)n);
    memcpy(b + 40, text, n); rb_put32(b + 24, crc(b + 40, n)); rb_put32(b + 28, crc(b, 28)); rb_put32(b + 32, 0x43524731u);
    return 40 + n;
}
static void put_slot(const char *p, uint32_t sequence, const char *text)
{
    uint8_t b[256]; size_t n = slot_bytes(b, sequence, text); char f[600]; path_of(f, p);
    FILE *o = fopen(f, "wb"); CHECK(o); if (!o) return;
    CHECK(fwrite(b, 1, n, o) == n); fclose(o);
}
static int slot_is(const char *p, uint32_t sequence, const char *text)
{
    uint8_t want[256], got[300]; size_t n = slot_bytes(want, sequence, text); char f[600]; path_of(f, p);
    FILE *i = fopen(f, "rb"); if (!i) return 0;
    size_t k = fread(got, 1, sizeof(got), i); fclose(i);
    return k == n && !memcmp(got, want, n);
}
static void dir(const char *p) { char f[600]; path_of(f, p); mkdir(f, 0755); }
static unsigned backups(void)
{
    unsigned n = 0; char f[600];
    for (unsigned i = 0; i < 100; i++) { snprintf(f, sizeof(f), "/System/SysBckup/RB%03u.CRB", i); n += exists(f); }
    return n;
}
static void tree(void)
{
    dir("/System"); dir("/System/Security"); dir("/System/Security/Registry"); dir(RC_MACHINE);
    dir("/system"); dir("/system/settings");
    put(RC_MACHINE "/System.reg", "; ciuki-registry generation=3\n[System\\Version]\nname = \"CiukiOS\"\n");
    put(RC_MACHINE "/Programs.reg", "; ciuki-registry generation=1\n[Programs\\Notepad]\nfont = \"Courier\"\n");
    put_slot(RC_MACHINE "/SYSTEM.CRG.0", 4, "[HKLM]\nhive\n");
    put("/system/BOOT.CFG", "safe=0 serial=1\n");
    put("/system/HWPROF.CFG", "0:name=Original Configuration\n");
    put("/system/settings/performance.cfg", "version=1\nrole=0\n");
}
static int request(int who, unsigned op, const void *tail, size_t tail_bytes, uint8_t *out, size_t cap, size_t *used)
{
    uint8_t in[32 + 64]; memset(in, 0, sizeof(in)); rb_put32(in, 1);
    if (tail_bytes) memcpy(in + 32, tail, tail_bytes);
    size_t got = 0;
    int e = rb_exchange(&broker, slot[who], op, in, 32 + tail_bytes, out, cap, &got);
    if (used) *used = got;
    return e;
}

static void test_administrator_only(void)
{
    struct rc_backup list[8], made;
    unsigned restored = 0; char from[16];
    /* Listing, backups, restores and repairs: administrators (the interim
     * session is one) and SYSTEM; never a standard user or Guest. */
    for (int who = 0; who < PEERS; who++) {
        client = who; int admin = who == ADMIN || who == INTERIM || who == SYSTEM;
        int n = rc_list("", list, 8);
        CHECK(admin ? n >= 0 : n == -EPERM);
        CHECK(admin || rc_backup("", "user", 0, true, &made) == -EPERM);
        CHECK(admin || rc_restore("", "RB000.CRB") == -EPERM);
        CHECK(admin || rc_autorestore("", &restored, from) == -EPERM);
        if (!admin) printf("H07 checker %s: LIST/BACKUP/RESTORE/FIX EPERM\n", peer_name[who]);
    }
    CHECK(!backups());
    puts("H07 sec-sysvc2 checker: Registry Checker's list, backup, restore and repair are refused to standard users and Guest (EPERM) PASS");
}

static void test_daily_job(void)
{
    int made = 0; struct rc_backup list[8];
    /* The daily backup is a SYSTEM job: a Guest's desktop asks, the broker
     * makes it by its own clock (the request carries no time). */
    const int64_t day = 1700006400;                                              /* 00:00 UTC */
    client = GUEST; clock_now = day + 10 * 3600; clock_offset = 0;
    CHECK(!rc_daily("", 5, 999, &made) && made == 1 && backups() == 1);
    client = STANDARD; clock_now = day + 20 * 3600;
    CHECK(!rc_daily("", 0, 0, &made) && made == 0);                             /* once a local day */
    client = INTERIM; CHECK(!rc_daily("", 0, 0, &made) && made == 0);
    client = SYSTEM; CHECK(!rc_daily("", 0, 0, &made) && made == 0);
    clock_now = day + 86400 + 10 * 3600; client = STANDARD;
    CHECK(!rc_daily("", 0, 0, &made) && made == 1 && backups() == 2);
    client = ADMIN; CHECK(rc_list("", list, 8) == 2 && !strcmp(list[0].comment, "Daily backup") && list[0].started &&
                          list[0].created == clock_now && list[0].files == 6 && list[0].valid);
    /* The local day is the broker's: at 23:00 UTC it is the same day in UTC,
     * the next one at UTC+2. */
    clock_now = day + 86400 + 23 * 3600; clock_offset = 0; client = GUEST;
    CHECK(!rc_daily("", 0, 0, &made) && made == 0);
    clock_offset = 120; CHECK(!rc_daily("", 0, 0, &made) && made == 1 && backups() == 3);
    puts("H07 sec-sysvc2 checker: the daily backup is the broker's SYSTEM job for Guest, standard user, interim and SYSTEM requests, once per local day by the broker's clock PASS");
}

static void test_backup_and_restore(void)
{
    struct rc_backup made, list[8];
    client = ADMIN; started = 0; clock_now += 60;
    CHECK(!rc_backup("", "Before the game", 0, true, &made) && !made.started && made.files == 6 && made.created == clock_now);
    CHECK(rc_list("", list, 8) == 4 && !strcmp(list[0].name, made.name) && !strcmp(list[0].comment, "Before the game"));
    started = 1;
    /* The comment is checked by the broker: printable, terminated. */
    uint8_t out[256]; char tail[RC_COMMENT]; memset(tail, 'a', sizeof(tail));
    CHECK(request(ADMIN, RB_CHECKER_BACKUP, tail, sizeof(tail), out, sizeof(out), NULL) == -EINVAL);
    memset(tail, 0, sizeof(tail)); tail[0] = 7; CHECK(request(ADMIN, RB_CHECKER_BACKUP, tail, sizeof(tail), out, sizeof(out), NULL) == -EINVAL);
    CHECK(backups() == 4);
    /* A restore is recorded for the next start; nothing changes now. */
    put("/system/BOOT.CFG", "safe=1\n");
    put(RC_MACHINE "/System.reg", "; ciuki-registry generation=9\n[System\\Version]\nname = \"Changed\"\n");
    put_slot(RC_MACHINE "/SYSTEM.CRG.1", 8, "[HKLM]\nnewer\n");             /* would win at the next load */
    client = GUEST; CHECK(rc_restore("", made.name) == -EPERM && !exists("/System/SysBckup/RESTORE.PND"));
    client = ADMIN;
    CHECK(rc_restore("", "../RB000.CRB") == -EINVAL && rc_restore("", "RB077.CRB") == -ENOENT);
    char name[16]; memset(name, 'x', sizeof(name));
    CHECK(request(ADMIN, RB_CHECKER_RESTORE, name, sizeof(name), out, sizeof(out), NULL) == -EINVAL);
    CHECK(!rc_restore("", made.name) && exists("/System/SysBckup/RESTORE.PND"));
    CHECK(!strcmp(get("/system/BOOT.CFG"), "safe=1\n") && strstr(get(RC_MACHINE "/System.reg"), "Changed"));
    struct rc_status st;
    client = GUEST; CHECK(!rc_status("", &st) && !strcmp(st.pending, made.name) && !st.applied[0] && !st.damaged);
    /* The broker's next start applies it before anything is loaded. */
    rb_checker_boot(&checker);
    CHECK(!strcmp(checker.applied, made.name) && !checker.restored && !checker.boot_error);
    CHECK(!strcmp(get("/system/BOOT.CFG"), "safe=0 serial=1\n") && strstr(get(RC_MACHINE "/System.reg"), "CiukiOS"));
    CHECK(slot_is(RC_MACHINE "/SYSTEM.CRG.0", 4, "[HKLM]\nhive\n") && !exists(RC_MACHINE "/SYSTEM.CRG.1"));
    CHECK(!strcmp(get("/system/HWPROF.CFG"), "0:name=Original Configuration\n"));
    client = STANDARD; CHECK(!rc_status("", &st) && !strcmp(st.applied, made.name) && !st.pending[0]);
    /* The restored root's generation passed the replaced one (9). */
    char *text = NULL; size_t n = 0; char p[600]; path_of(p, RC_MACHINE "/System.reg");
    CHECK(!reg_file_read(p, &text, &n)); struct rt *t = text ? reg_parse("System", text, n) : NULL;
    CHECK(t && t->generation == 10); reg_tree_free(t); free(text);
    rb_checker_boot(&checker); CHECK(!checker.applied[0]);                     /* once */
    puts("H07 sec-sysvc2 checker: administrator backup (broker's started flag, checked comment), restore recorded for the next start and applied by the broker before loading (roots at a higher generation, hive slots, BOOT.CFG, HWPROF.CFG), once PASS");
}

static void test_damaged_roots(void)
{
    struct rc_status st; unsigned restored = 0; char from[16];
    struct rc_backup list[8];
    client = ADMIN; CHECK(rc_list("", list, 8) >= 1);
    /* A damaged root at the start: restored from the newest good backup,
     * reported to every session's desktop through STATUS. */
    put(RC_MACHINE "/System.reg", "[System\\Version\nbroken");
    client = GUEST; CHECK(!rc_status("", &st) && st.damaged == 1);
    unsigned damaged = 0; CHECK(!rc_scan("", &damaged) && damaged == 1);
    rb_checker_boot(&checker);
    CHECK(checker.restored == 1 && !strcmp(checker.from, list[0].name));
    client = STANDARD; CHECK(!rc_status("", &st) && !st.damaged && st.restored == 1 && !strcmp(st.from, list[0].name));
    CHECK(exists(RC_MACHINE "/System.reg.bad") && strstr(get(RC_MACHINE "/System.reg"), "CiukiOS"));
    /* SCANREG /FIX: administrators and SYSTEM, at once. */
    put(RC_MACHINE "/Programs.reg", "\x01 not a registry");
    client = GUEST; CHECK(rc_autorestore("", &restored, from) == -EPERM);
    client = SYSTEM; CHECK(!rc_autorestore("", &restored, from) && restored == 2 && !strcmp(from, list[0].name));
    CHECK(strstr(get(RC_MACHINE "/Programs.reg"), "Courier"));
    /* A Win32 machine hive without a valid slot would stop the broker's
     * loader: it is restored at the broker's start, never under a running one. */
    put(RC_MACHINE "/SYSTEM.CRG.0", "damaged"); put(RC_MACHINE "/SYSTEM.CRG.1", "damaged too");
    client = SYSTEM; CHECK(!rc_autorestore("", &restored, from) && !restored && !strcmp(get(RC_MACHINE "/SYSTEM.CRG.0"), "damaged"));
    rb_checker_boot(&checker);
    CHECK(checker.restored == RC_HIVE && !strcmp(checker.from, list[0].name) && !checker.boot_error);
    CHECK(slot_is(RC_MACHINE "/SYSTEM.CRG.0", 4, "[HKLM]\nhive\n") && !exists(RC_MACHINE "/SYSTEM.CRG.1") && exists(RC_MACHINE "/SYSTEM.CRG.1.bad"));
    client = GUEST; CHECK(!rc_status("", &st) && st.restored == RC_HIVE);
    puts("H07 sec-sysvc2 checker: a damaged root is restored by the broker at its start and reported to every session (STATUS); SCANREG /FIX for administrators and SYSTEM only; a Win32 machine hive without a valid slot is restored at the broker's start only PASS");
}

static void test_wire(void)
{
    uint8_t out[RB_BODY_MAX]; size_t used = 0;
    uint8_t in[32]; memset(in, 0, sizeof(in));
    rb_put32(in, 2); CHECK(rb_exchange(&broker, slot[ADMIN], RB_CHECKER_LIST, in, 32, out, sizeof(out), &used) == -EINVAL);
    rb_put32(in, 1); in[20] = 1; CHECK(rb_exchange(&broker, slot[ADMIN], RB_CHECKER_LIST, in, 32, out, sizeof(out), &used) == -EINVAL);
    in[20] = 0; CHECK(rb_exchange(&broker, slot[ADMIN], RB_CHECKER_LIST, in, 31, out, sizeof(out), &used) == -EINVAL);
    CHECK(rb_exchange(&broker, slot[ADMIN], RB_CHECKER_DAILY, in, 33, out, sizeof(out), &used) == -EINVAL);
    CHECK(rb_exchange(&broker, slot[ADMIN], RB_CHECKER_STATUS, in, 32, out, 40, &used) == -EINVAL);
    CHECK(!rb_exchange(&broker, slot[ADMIN], RB_CHECKER_LIST, in, 32, out, sizeof(out), &used) && used == 32 + rb_get32(out + 4) * RC_WIRE);
    /* A revoked peer is refused before any work. */
    live[GUEST] = 0; CHECK(rb_exchange(&broker, slot[GUEST], RB_CHECKER_STATUS, in, 32, out, sizeof(out), &used) == -ESRCH); live[GUEST] = 1;
    /* Without Registry Checker the broker refuses the operations. */
    struct rb_checker *saved = broker.checker; broker.checker = NULL;
    CHECK(rb_exchange(&broker, slot[ADMIN], RB_CHECKER_STATUS, in, 32, out, sizeof(out), &used) == -EOPNOTSUPP);
    broker.checker = saved;
    /* The backups stay with the broker: nothing of them crossed to a client but records. */
    puts("H07 sec-sysvc2 checker: wire header/tail validation, reply capacity, revoked peer, broker without Registry Checker PASS");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    root = argv[1]; checker.root = root;
    tree();
    const struct { uint32_t uid, session, groups[2], count; } who[PEERS] = {
        { 1000, 1, { 544, 545 }, 2 }, { 1001, 2, { 545 }, 1 }, { 501, 3, { 546 }, 1 }, { 1000, 4, { 544, 545 }, 2 }, { 0, 0, { 0 }, 0 } };
    for (int i = 0; i < PEERS; i++) {
        peers[i] = (struct rb_identity){ .uid = who[i].uid, .session = who[i].session, .group_count = who[i].count,
            .process_generation = 1, .machine_sid = { 1, 2, 3 }, .volume = 42, .account_generation = 1 };
        memcpy(peers[i].groups, who[i].groups, sizeof(who[i].groups));
        if (i == ADMIN) strcpy(peers[i].profile, "/Windows/Profiles/u000003e8");
        if (i == STANDARD) strcpy(peers[i].profile, "/Windows/Profiles/u000003e9");
        if (i == GUEST) strcpy(peers[i].profile, "/Windows/Profiles/u000001f5");
    }
    struct rb_authority a = { NULL, role, retain, release, inspect };
    struct rb_storage s = { .read = storage_read, .write = storage_write };
    CHECK(!rb_init(&broker, &a, &s, NULL));
    broker.checker = &checker;
    rb_checker_boot(&checker);
    CHECK(!checker.restored && !checker.applied[0] && !checker.boot_error && log_lines == 1);
    for (int i = 0; i < PEERS; i++) CHECK(!rb_connect(&broker, (uintptr_t)i + 1, &slot[i]));
    test_administrator_only();
    test_daily_job();
    test_backup_and_restore();
    test_damaged_roots();
    test_wire();
    for (int i = 0; i < PEERS; i++) rb_disconnect(&broker, slot[i]);
    rb_destroy(&broker);
    printf("H07 sec-sysvc2 Registry Checker broker: checks=%u failures=%u\n", checks, failures);
    if (!failures) puts("H07 sec-sysvc2 Registry Checker broker: PASS");
    return failures != 0;
}
