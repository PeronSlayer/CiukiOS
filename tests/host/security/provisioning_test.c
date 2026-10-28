/* SPDX-License-Identifier: MIT
 * H08 (sec-user-02): Setup/ready provisioning, first-boot promotion, G32
 * migration evidence and enrollment, and the confirmed Setup CD repair.
 * Links the production account service core, the first-boot adapter and
 * Setup's seed/repair module with the fault-injecting fake store; every
 * durable write/flush/barrier is cut in turn and recovery is checked.
 */
#include "../../../apps/setup/setup.h"  /* first: its ABI header precedes the C library's time.h */
#include "fake.h"
#include "wire.h"
#include "firstboot.h"
#include "../../../apps/desktop/users.h"
#include <ciuki/sha256.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOLUME 1234u /* fake_store() volume */

static int test_entropy(void *ctx, void *out, size_t n)
{
    unsigned *state = ctx;
    if (!state) return -EAGAIN;
    for (size_t i = 0; i < n; ++i) ((uint8_t *)out)[i] = (uint8_t)(++*state * 151u + 7u);
    return 0;
}
static void hash(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }
static int present(struct fake *f, const char *name) { return fake_file_index(f, name) >= 0; }
static size_t file_bytes(struct fake *f, const char *name, void *out, size_t cap)
{
    int i = fake_file_index(f, name); assert(i >= 0 && f->files[i].size <= cap);
    memcpy(out, f->files[i].data, f->files[i].size); return f->files[i].size;
}
static size_t ready_seed(uint8_t out[ACCT_FILE_MAX])
{
    struct acct_db d; fake_seed(&d, 1); size_t n = 0;
    assert(!acct_encode(&d, 1, out, ACCT_FILE_MAX, &n) && n == CIUKI_PROVISION_SEED_BYTES);
    return n;
}
static size_t setup_seed(uint8_t out[ACCT_FILE_MAX], const char *name)
{
    unsigned state = 40;
    assert(!st_seed_build(out, name, "alicepass1", "recovery12", test_entropy, &state));
    return CIUKI_PROVISION_SEED_BYTES;
}
/* What Builder/Setup put on a destination: the seed and its UNPROVISIONED record. */
static void stage(struct fake *f, const uint8_t *seed, size_t n, uint32_t origin)
{
    uint8_t digest[32], state[CIUKI_PROVISION_STATE_BYTES];
    hash(seed, n, digest);
    assert(!ciuki_provision_state(state, origin, VOLUME, digest, hash));
    fake_put(f, "provision.seed", seed, n);
    fake_put(f, "bootstrap.state", state, sizeof(state));
}
/* The image's ACCOUNT start sequence (server.c): prepare, open, promote. */
static int boot(struct fake *f, struct acct_service *s, const struct acct_legacy_io *legacy, int *promoted)
{
    struct acct_store st = fake_store(f);
    int e = acct_firstboot_prepare(&st, legacy);
    if (e < 0) return e;
    e = acct_service_open(s, &st, 1);
    if (e) return e;
    int r = s->available ? ACCT_FIRSTBOOT_NOTHING : acct_firstboot_promote(s);
    if (promoted) *promoted = r;
    if (r < 0) { acct_service_close(s); return r; }
    return 0;
}

/* ---- codec: Python builder bytes against the account codecs ---- */
static void codec(const char *seed_path, const char *state_path, unsigned long long volume)
{
    uint8_t python_seed[ACCT_FILE_MAX + 1], python_state[200], c_seed[ACCT_FILE_MAX];
    FILE *fp = fopen(seed_path, "rb"); assert(fp); size_t sn = fread(python_seed, 1, sizeof(python_seed), fp); fclose(fp);
    fp = fopen(state_path, "rb"); assert(fp); size_t bn = fread(python_state, 1, sizeof(python_state), fp); fclose(fp);
    assert(sn == CIUKI_PROVISION_SEED_BYTES && bn == CIUKI_PROVISION_STATE_BYTES);
    assert(ready_seed(c_seed) == sn && !memcmp(c_seed, python_seed, sn));
    struct acct_db d; assert(!acct_decode(&d, 1, python_seed, sn));
    assert(d.origin == 1 && d.auto_logon_uid == 1000 && d.count == 3 && d.records[2].flags == 9 && !strcmp(d.records[2].name, "CiukiUser"));
    assert(!ciuki_provision_seed_header(python_seed, sn, 1) && ciuki_provision_seed_header(python_seed, sn, 2));
    struct acct_bootstrap b; assert(!acct_boot_decode(&b, python_state, bn));
    uint8_t digest[32]; hash(python_seed, sn, digest);
    assert(b.state == 1 && b.origin == 1 && b.volume == volume && b.next_uid == 1001 && b.generation == 1 && !b.flags &&
           !b.transaction && !b.accounts_generation && !b.owner_generation && !memcmp(b.seed_digest, digest, 32));
    uint8_t again[CIUKI_PROVISION_STATE_BYTES], encoded[CIUKI_PROVISION_STATE_BYTES];
    assert(!ciuki_provision_state(again, 1, volume, digest, hash) && !memcmp(again, python_state, bn));
    assert(!acct_boot_encode(&b, encoded, sizeof(encoded)) && !memcmp(encoded, python_state, bn));
    for (size_t i = 0; i < sn; ++i) { python_seed[i] ^= 0x40; assert(acct_decode(&d, 1, python_seed, sn)); python_seed[i] ^= 0x40; }
    for (size_t i = 0; i < bn; ++i) { python_state[i] ^= 0x40; assert(acct_boot_decode(&b, python_state, bn)); python_state[i] ^= 0x40; }
    /* Setup's recipe: exact records, independent salts, every refusal before output. */
    uint8_t setup[ACCT_FILE_MAX]; setup_seed(setup, "Alice");
    assert(!acct_decode(&d, 1, setup, CIUKI_PROVISION_SEED_BYTES) && d.origin == 2 && !d.auto_logon_uid);
    assert(d.records[0].flags == 6 && d.records[1].flags == 2 && d.records[2].flags == 5 && !strcmp(d.records[2].name, "Alice"));
    assert(memcmp(d.records[0].salt, d.records[2].salt, 16) && acct_verify(&d.records[2], "alicepass1") && acct_verify(&d.records[0], "recovery12"));
    assert(!acct_verify(&d.records[0], "alicepass1") && !acct_verify(&d.records[2], ""));
    static const char *const refused[] = {"CiukiUser", "ciukiuser", "Guest", "SYSTEM", "Administrators", "CiukiAdministrator", "Users", "Guests", "", "bad name"};
    unsigned state = 1;
    for (unsigned i = 0; i < sizeof(refused) / sizeof(*refused); ++i)
        assert(st_seed_build(setup, refused[i], "alicepass1", "recovery12", test_entropy, &state) == -EINVAL);
    assert(st_seed_build(setup, "Alice", "short", "recovery12", test_entropy, &state) == -EINVAL);
    assert(st_seed_build(setup, "Alice", "alicepass1", "1234567", test_entropy, &state) == -EINVAL);
    char long_password[65]; memset(long_password, 'x', 64); long_password[64] = 0;
    assert(st_seed_build(setup, "Alice", long_password, "recovery12", test_entropy, &state) == -EINVAL);
    assert(st_seed_build(setup, "Alice", "alicepass1", "recovery12", test_entropy, NULL) == -EAGAIN);
    for (unsigned i = 0; i < CIUKI_PROVISION_SEED_BYTES; ++i) assert(!setup[i]); /* nothing survives a refusal */
    puts("PASS H08 codec: Python ready seed/state equal the account codec and provision.h, every corrupted byte refused, exact Setup recipe with independent salts, reserved names/policy/entropy refusals");
}

/* ---- promotion of ready and Setup seeds with every cut ---- */
struct outcome { unsigned recovered, closed, boundaries; };
static void check_promoted(struct fake *f, struct acct_service *s, int ready)
{
    assert(s->available && s->db.count == 3 && s->db.origin == (ready ? 1u : 2u) && s->db.generation >= 1);
    const struct acct_record *r = s->db.records;
    assert(r[0].uid == 500 && !(r[0].flags & ACCT_ENABLED) && r[1].uid == 501 && r[1].flags == ACCT_BUILTIN && !r[1].kdf);
    if (ready) {
        assert(r[0].flags == ACCT_BUILTIN && !r[0].kdf && !strcmp(r[2].name, "CiukiUser") && r[2].flags == 13);
        assert(s->db.auto_logon_uid == 1000 && acct_verify(&r[2], "") && !acct_verify(&r[0], ""));
    } else {
        assert(r[0].flags == 6 && acct_verify(&r[0], "recovery12") && !strcmp(r[2].name, "Alice") && r[2].flags == 5);
        assert(!s->db.auto_logon_uid && acct_verify(&r[2], "alicepass1"));
        for (unsigned i = 0; i < 3; ++i) assert(strcmp(r[i].name, "CiukiUser"));
    }
    struct acct_bootstrap b; assert(!acct_boot_read(&s->store, &b) && b.state == ACCT_CONSUMED && (b.flags & 1));
    assert(!memcmp(b.sid, s->db.sid, 12) && !azero(b.sid, 12) && b.accounts_generation == s->db.generation);
    uint8_t sid[65]; size_t n = file_bytes(f, "machine.sid", sid, sizeof(sid)); assert(n == 64);
    for (unsigned i = 0; i < 3; ++i) assert(a32(sid + 8 + 4 * i) == s->db.sid[i]);
    assert(acct_autologon_eligible(&s->db, &b, 0, 0, 0) == ready);
}
static struct outcome promotion(int ready)
{
    uint8_t seed[ACCT_FILE_MAX]; size_t n = ready ? ready_seed(seed) : setup_seed(seed, "Alice");
    struct fake f; fake_init(&f); stage(&f, seed, n, ready ? 1 : 2);
    struct acct_service s; int promoted = 0; unsigned before = f.event;
    assert(!boot(&f, &s, NULL, &promoted) && promoted == ACCT_FIRSTBOOT_PROMOTED);
    unsigned events = f.event - before;
    check_promoted(&f, &s, ready);
    assert(!present(&f, "provision.seed")); /* removed after CONSUMED */
    uint8_t first_sid[12]; memcpy(first_sid, s.db.sid, 12);
    /* Restart: no promotion; a residual seed is never replayed. */
    acct_service_close(&s); fake_powercut(&f); fake_put(&f, "provision.seed", seed, n);
    assert(!boot(&f, &s, NULL, &promoted) && promoted == ACCT_FIRSTBOOT_NOTHING);
    check_promoted(&f, &s, ready); assert(!memcmp(first_sid, s.db.sid, 12));
    acct_service_close(&s); fake_destroy(&f);
    struct outcome o = { 0, 0, events };
    for (unsigned cut = 1; cut <= events; ++cut) {
        fake_init(&f); stage(&f, seed, n, ready ? 1 : 2); f.cut = f.event + cut;
        if (!boot(&f, &s, NULL, &promoted)) acct_service_close(&s);
        fake_powercut(&f);
        /* A durable SID intent is the installation identity: never replaced. */
        uint8_t intent[129]; int sealed = present(&f, "bootstrap.sid") && file_bytes(&f, "bootstrap.sid", intent, sizeof(intent)) == 128;
        int e = boot(&f, &s, NULL, &promoted);
        if (!e && !s.available) { acct_service_close(&s); fake_powercut(&f); e = boot(&f, &s, NULL, &promoted); }
        if (!e) {
            check_promoted(&f, &s, ready); ++o.recovered;
            if (sealed) for (unsigned k = 0; k < 3; ++k) assert(a32(intent + 24 + 4 * k) == s.db.sid[k]);
            acct_service_close(&s);
        } else {
            /* Closed for repair: never a published database or second identity. */
            assert(e == -EIO || e == -ENOENT); ++o.closed;
            if (getenv("H08_TRACE")) {
                printf("  closed cut %u (e=%d):", cut, e);
                for (unsigned i = 0; i < FAKE_FILES; ++i) if (f.files[i].name[0]) printf(" %s", f.files[i].name);
                struct acct_bootstrap b; struct acct_store st = fake_store(&f);
                if (!acct_boot_read(&st, &b)) printf(" [state=%u]", b.state);
                putchar('\n');
            }
            assert(!present(&f, "accounts.db") && !present(&f, "accounts.db.new"));
        }
        fake_destroy(&f);
    }
    return o;
}

/* ---- missing/contradictory evidence: closed, nothing published ---- */
static void contradictory(void)
{
    uint8_t ready[ACCT_FILE_MAX], setup[ACCT_FILE_MAX]; ready_seed(ready); setup_seed(setup, "Alice");
    uint8_t state[CIUKI_PROVISION_STATE_BYTES], digest[32];
    for (unsigned kind = 0; kind < 7; ++kind) {
        struct fake f; fake_init(&f); struct acct_service s; int promoted = -1;
        hash(ready, CIUKI_PROVISION_SEED_BYTES, digest);
        switch (kind) {
        case 0: fake_put(&f, "provision.seed", ready, CIUKI_PROVISION_SEED_BYTES); break;           /* seed without state */
        case 1: assert(!ciuki_provision_state(state, 1, VOLUME, digest, hash)); fake_put(&f, "bootstrap.state", state, sizeof(state)); break; /* state without seed */
        case 2: stage(&f, ready, CIUKI_PROVISION_SEED_BYTES, 1); fake_put(&f, "provision.seed", setup, CIUKI_PROVISION_SEED_BYTES); break; /* other seed */
        case 3: stage(&f, ready, CIUKI_PROVISION_SEED_BYTES, 2); break;                             /* ready seed, Setup state */
        case 4: assert(!ciuki_provision_state(state, 1, VOLUME + 1, digest, hash));               /* other volume */
                fake_put(&f, "provision.seed", ready, CIUKI_PROVISION_SEED_BYTES); fake_put(&f, "bootstrap.state", state, sizeof(state)); break;
        case 5: stage(&f, ready, CIUKI_PROVISION_SEED_BYTES, 1); fake_put(&f, "accounts.alloc.0", ready, 96); break; /* prior reservation */
        case 6: break;                                                                             /* nothing at all */
        }
        int e = boot(&f, &s, NULL, &promoted);
        if (!e) { assert(!s.available); acct_service_close(&s); }
        assert(!present(&f, "accounts.db") && !present(&f, "machine.sid"));
        fake_destroy(&f);
    }
    /* After consumption, losing accounts.db never re-provisions from a residual seed. */
    struct fake f; fake_init(&f); struct acct_service s; int promoted = 0;
    stage(&f, ready, CIUKI_PROVISION_SEED_BYTES, 1); assert(!boot(&f, &s, NULL, &promoted)); acct_service_close(&s);
    fake_powercut(&f); fake_put(&f, "provision.seed", ready, CIUKI_PROVISION_SEED_BYTES);
    int i = fake_file_index(&f, "accounts.db"); f.files[i].name[0] = f.files[i].durable_name[0] = 0;
    assert(boot(&f, &s, NULL, &promoted) == -EIO && !present(&f, "accounts.db"));
    fake_destroy(&f);
    puts("PASS H08 contradictory evidence: seed/state/volume/origin/digest mismatch, prior reservation and lost database stay closed; nothing published, no SID minted, no consumed seed replay");
}

/* ---- G32 migration evidence, enrollment and wipe ---- */
struct legacy { uint8_t data[3][ACCT_LEGACY_MAX + 1]; size_t size[3]; int exists[3]; unsigned removes, barriers; int fail_read; };
static int legacy_index(const char *name)
{ return !strcmp(name, "users.dat") ? 0 : !strcmp(name, "users.new") ? 1 : !strcmp(name, "users.lock") ? 2 : -1; }
static int legacy_read(void *ctx, const char *name, void *out, size_t cap, size_t *n)
{
    struct legacy *l = ctx; int i = legacy_index(name); assert(i >= 0);
    if (l->fail_read) return -EIO;
    if (!l->exists[i]) return -ENOENT;
    size_t k = l->size[i] < cap ? l->size[i] : cap; memcpy(out, l->data[i], k); *n = k; return 0;
}
static int legacy_remove(void *ctx, const char *name)
{ struct legacy *l = ctx; int i = legacy_index(name); assert(i >= 0); if (!l->exists[i]) return -ENOENT; l->exists[i] = 0; l->removes++; return 0; }
static int legacy_barrier(void *ctx) { ((struct legacy *)ctx)->barriers++; return 0; }
static size_t g32_store(uint8_t out[1400], unsigned count)
{
    struct users_db old = { .count = count, .next_id = 10 };
    static const char *const names[] = {"Mario", "Luigi", "Peach"};
    for (unsigned i = 0; i < count; ++i) {
        old.records[i].id = 3 + 2 * i; strcpy(old.records[i].name, names[i]);
        users_derive(i == 1 ? "abc" : "oldpass1", old.records[i].salt, 10000, old.records[i].verifier);
    }
    size_t n = users_encode(&old, out, 1400); assert(n); return n;
}
static void legacy_init(struct legacy *l, unsigned users)
{
    memset(l, 0, sizeof(*l)); l->size[0] = g32_store(l->data[0], users); l->exists[0] = 1;
    memcpy(l->data[1], "partial", 7); l->size[1] = 7; l->exists[1] = 1;
    memcpy(l->data[2], "1234", 4); l->size[2] = 4; l->exists[2] = 1;  /* the G32 lock's holder record */
}
static void check_evidence(struct fake *f, const struct legacy *original)
{
    uint8_t bytes[ACCT_LEGACY_MAX + 1], digest[32], state[CIUKI_PROVISION_STATE_BYTES + 1];
    size_t n = file_bytes(f, ACCT_MIGRATION_SOURCE, bytes, sizeof(bytes));
    assert(n == original->size[0] && !memcmp(bytes, original->data[0], n));
    n = file_bytes(f, ACCT_MIGRATION_PREFIX "users.new", bytes, sizeof(bytes)); assert(n == 7 && !memcmp(bytes, "partial", 7));
    assert(present(f, ACCT_MIGRATION_PREFIX "users.lock") && present(f, ACCT_MIGRATION_MARKER));
    struct acct_bootstrap b; n = file_bytes(f, "bootstrap.state", state, sizeof(state)); assert(!acct_boot_decode(&b, state, n));
    hash(original->data[0], original->size[0], digest);
    assert(b.origin == 3 && b.volume == VOLUME && !memcmp(b.seed_digest, digest, 32));
}
static void enroll_all(struct fake *f, struct acct_service *s, int first_old_id)
{
    struct acct_request q = { .operation = ACCT_ENROLL_PLAN, .service = 1, .wire_operation = 7, .plan_count = 3, .first_old_id = (uint32_t)first_old_id };
    static const char *const names[] = {"Mario", "Luigi", "Peach"};
    for (unsigned i = 0; i < 3; ++i) { q.plan[i].old_id = 3 + 2 * i; strcpy(q.plan[i].name, names[i]); }
    struct acct_response r; assert(!acct_submit(s, 0, &q, sizeof(q), &r)); assert(acct_work(s, &r) == 1 && !r.error && r.count == 5);
    f->enrollment_count = s->enrollment.map_count;
    for (unsigned i = 0; i < f->enrollment_count; ++i) {
        f->enrollment[i].legacy_id = s->enrollment.map[i].old_id; f->enrollment[i].uid = s->enrollment.map[i].uid;
        memcpy(f->enrollment[i].root, s->enrollment.map[i].root, 64);
    }
    unsigned count = s->enrollment.draft.count;
    for (unsigned i = 0; i < count; ++i) if (i != 1) {
        const struct acct_record *rec = &s->enrollment.draft.records[i];
        fake_request(&q, ACCT_ENROLL_VERIFY, s->enrollment.generation, rec->uid, NULL, "newpass12");
        if (rec->legacy_id) strcpy(q.old_password, rec->legacy_id == 5 ? "abc" : "oldpass1");
        assert(!fake_run(s, 0, &q, &r)); f->now += 1000;
    }
}
static int owner_unbound(void *ctx, uint64_t *g) { (void)ctx; (void)g; return -ENOSYS; }
static void migration(void)
{
    struct legacy l, original; legacy_init(&l, 3); original = l;
    struct acct_legacy_io io = { &l, legacy_read, legacy_remove, legacy_barrier };
    struct fake f; fake_init(&f); struct acct_service s; int promoted = 0;
    assert(!boot(&f, &s, &io, &promoted) && promoted == ACCT_FIRSTBOOT_ENROLLMENT && !s.available);
    check_evidence(&f, &original);
    assert(!l.exists[0] && !l.exists[1] && !l.exists[2]);                 /* no shared verifier copy remains */
    assert(present(&f, "enrollment.source") && !present(&f, "accounts.db"));
    /* The trusted enrollment workflow alone publishes: Luigi (old id 5) first administrator. */
    enroll_all(&f, &s, 5);
    assert(s.available && s.db.count == 5 && s.db.origin == 3 && (s.db.flags & ACCT_MIGRATED));
    const struct acct_record *r = s.db.records;
    assert(r[2].uid == 1000 && !strcmp(r[2].name, "Luigi") && r[2].groups == ACCT_ADMIN && acct_verify(&r[2], "newpass12"));
    assert(r[3].groups == ACCT_STANDARD && r[4].groups == ACCT_STANDARD && r[3].legacy_id == 3 && r[4].legacy_id == 7);
    acct_service_close(&s); fake_powercut(&f);
    /* Restart, and a restored shared users.dat: removed again, never reimported. */
    l.exists[0] = 1;
    assert(!boot(&f, &s, &io, &promoted) && s.available && s.db.count == 5 && !l.exists[0]);
    acct_service_close(&s); fake_powercut(&f);
    /* Losing every protected file after migration leaves no G32 evidence: closed, no fresh administrator path. */
    for (unsigned i = 0; i < FAKE_FILES; ++i) f.files[i].name[0] = f.files[i].durable_name[0] = 0;
    assert(boot(&f, &s, &io, &promoted) == -EIO && !present(&f, "bootstrap.state"));
    fake_destroy(&f);
    /* The interim image (OWNER unbound, every call ENOSYS) never moves the G32 files. */
    legacy_init(&l, 2); fake_init(&f); { struct acct_store un = fake_store(&f); un.owner.generation = owner_unbound;
      assert(acct_firstboot_prepare(&un, &io) == ACCT_FIRSTBOOT_NOTHING && !f.event && l.exists[0] && !l.removes); } fake_destroy(&f);
    /* No G32 store: nothing is written. Corrupt store: originals kept, nothing written. */
    struct legacy none; memset(&none, 0, sizeof(none)); struct acct_legacy_io noio = { &none, legacy_read, legacy_remove, legacy_barrier };
    fake_init(&f); assert(boot(&f, &s, &noio, &promoted) == -EIO && !f.event); fake_destroy(&f);
    struct legacy bad; legacy_init(&bad, 2); bad.data[0][40] ^= 1; struct acct_legacy_io badio = { &bad, legacy_read, legacy_remove, legacy_barrier };
    fake_init(&f); assert(boot(&f, &s, &badio, &promoted) == -EIO && !f.event && bad.exists[0] && !bad.removes); fake_destroy(&f);
    /* v2 evidence beside a G32 store is contradictory: nothing imported or removed. */
    legacy_init(&l, 2); fake_init(&f); fake_put(&f, "machine.sid", "x", 1); unsigned before = f.event;
    assert(boot(&f, &s, &io, &promoted) == -EIO && f.event == before && l.exists[0] && !l.removes); fake_destroy(&f);
    /* An empty G32 store requires one new custom administrator. */
    legacy_init(&l, 0); fake_init(&f);
    assert(!boot(&f, &s, &io, &promoted) && promoted == ACCT_FIRSTBOOT_ENROLLMENT);
    struct acct_request q = { .operation = ACCT_ENROLL_PLAN, .service = 1, .wire_operation = 7, .plan_count = 1 };
    strcpy(q.plan[0].name, "Daisy"); struct acct_response rr;
    assert(!acct_submit(&s, 0, &q, sizeof(q), &rr)); assert(acct_work(&s, &rr) == 1 && !rr.error && rr.count == 3);
    acct_service_close(&s); fake_destroy(&f);
    /* Every cut of the preparation resumes from the protected evidence. */
    legacy_init(&l, 3); original = l; fake_init(&f); before = f.event;
    struct acct_store st = fake_store(&f); assert(acct_firstboot_prepare(&st, &io) == ACCT_FIRSTBOOT_PREPARED);
    unsigned events = f.event - before, resumed = 0, closed = 0; fake_destroy(&f);
    for (unsigned cut = 1; cut <= events; ++cut) {
        legacy_init(&l, 3); fake_init(&f); f.cut = f.event + cut; st = fake_store(&f);
        assert(acct_firstboot_prepare(&st, &io) < 0);
        fake_powercut(&f); st = fake_store(&f);
        int e = acct_firstboot_prepare(&st, &io);
        if (e == ACCT_FIRSTBOOT_PREPARED) {
            check_evidence(&f, &original); assert(!l.exists[0]); ++resumed;
            assert(!boot(&f, &s, &io, &promoted) && promoted == ACCT_FIRSTBOOT_ENROLLMENT); acct_service_close(&s);
        } else { assert(e == -EIO); ++closed; assert(!present(&f, "bootstrap.state")); }
        fake_destroy(&f);
    }
    assert(resumed == events);
    printf("PASS H08 G32 migration: SYSTEM-only evidence, origin-3 state, shared copies removed, enrollment-only publication with chosen administrator and Standard imports, restored copy removed, wipe/empty/corrupt/contradictory sources, all %u preparation cuts resumed (%u closed)\n", events, closed);
}

/* ---- confirmed Setup CD repair ---- */
static void provisioned(struct fake *f)
{
    uint8_t seed[ACCT_FILE_MAX]; size_t n = setup_seed(seed, "Alice");
    fake_init(f); stage(f, seed, n, 2); struct acct_service s; int promoted = 0;
    assert(!boot(f, &s, NULL, &promoted) && promoted == ACCT_FIRSTBOOT_PROMOTED);
    acct_service_close(&s); fake_powercut(f);
}
static void repair(void)
{
    struct fake f; provisioned(&f); struct acct_store st = fake_store(&f); struct st_repair info;
    unsigned before = f.event;
    assert(!st_repair_inspect(&st.io, &info) && f.event == before && info.generation == 1 && info.count == 3 && info.recovery_configured);
    unsigned entropy = 500;
    assert(st_repair_execute(&st.io, "short", test_entropy, &entropy, &info) == -EINVAL && f.event == before);
    assert(st_repair_execute(&st.io, "freshrecovery", test_entropy, NULL, &info) == -EAGAIN);
    assert(!present(&f, "accounts.local"));
    before = f.event;
    assert(!st_repair_execute(&st.io, "freshrecovery", test_entropy, &entropy, &info) && info.next_generation == info.high_generation + 2);
    unsigned events = f.event - before;
    assert(st_repair_inspect(&st.io, &info) == -EBUSY && info.pending);  /* waits for the installed boot */
    struct acct_service s; assert(!acct_service_open(&s, &st, 1) && s.available);
    assert(s.db.generation == 3 && acct_verify(&s.db.records[0], "freshrecovery") && !(s.db.records[0].flags & ACCT_ENABLED));
    assert(acct_verify(&s.db.records[2], "alicepass1") && !present(&f, "accounts.local"));
    struct acct_bootstrap b; assert(!acct_boot_read(&s.store, &b) && b.accounts_generation == 3 && b.state == ACCT_CONSUMED);
    acct_service_close(&s); fake_destroy(&f);
    /* Refusals: unfinished first boot, unfinished change, damaged database, no installation. */
    fake_init(&f); st = fake_store(&f); assert(st_repair_inspect(&st.io, &info) == -ENOENT); fake_destroy(&f);
    uint8_t seed[ACCT_FILE_MAX]; size_t n = setup_seed(seed, "Alice");
    fake_init(&f); stage(&f, seed, n, 2); st = fake_store(&f); assert(st_repair_inspect(&st.io, &info) == -EBUSY); fake_destroy(&f);
    provisioned(&f); fake_put(&f, "accounts.db.txn", "x", 1); st = fake_store(&f); assert(st_repair_inspect(&st.io, &info) == -EBUSY); fake_destroy(&f);
    provisioned(&f); { int i = fake_file_index(&f, "accounts.db"); f.files[i].data[150] ^= 1; f.files[i].durable_data[150] ^= 1; }
    st = fake_store(&f); before = f.event; assert(st_repair_inspect(&st.io, &info) == -EIO);
    assert(st_repair_execute(&st.io, "freshrecovery", test_entropy, &entropy, &info) == -EIO && f.event == before); fake_destroy(&f);
    /* Every cut: the installation opens with the old or the repaired database, never closed. */
    unsigned old_state = 0, repaired = 0;
    for (unsigned cut = 1; cut <= events; ++cut) {
        provisioned(&f); st = fake_store(&f); f.cut = f.event + cut; entropy = 500;
        assert(st_repair_execute(&st.io, "freshrecovery", test_entropy, &entropy, &info) < 0);
        fake_powercut(&f); st = fake_store(&f);
        assert(!acct_service_open(&s, &st, 1) && s.available);
        if (acct_verify(&s.db.records[0], "freshrecovery")) ++repaired;
        else {
            assert(acct_verify(&s.db.records[0], "recovery12")); ++old_state;
            acct_service_close(&s); st = fake_store(&f); entropy = 900;
            assert(!st_repair_execute(&st.io, "freshrecovery", test_entropy, &entropy, &info));
            assert(!acct_service_open(&s, &st, 1) && s.available && acct_verify(&s.db.records[0], "freshrecovery"));
        }
        assert(acct_verify(&s.db.records[2], "alicepass1") && s.db.count == 3);
        acct_service_close(&s); fake_destroy(&f);
    }
    printf("PASS H08 Setup CD repair: read-only inspection, local intent finished by installed ACCOUNT recovery, uid 500 stays disabled, refusals write nothing, all %u cuts open (%u repaired, %u old then repaired)\n", events, repaired, old_state);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 5 && !strcmp(argv[1], "codec")) { codec(argv[2], argv[3], strtoull(argv[4], NULL, 0)); return 0; }
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "promotion")) {
        for (int ready = 1; ready >= 0; --ready) {
            struct outcome o = promotion(ready);
            printf("PASS H08 %s seed promotion: exact published records, one machine SID, seed removed, no residual replay, all %u cuts (%u recovered, %u safely closed)\n",
                   ready ? "ready" : "Setup", o.boundaries, o.recovered, o.closed);
        }
    } else if (!strcmp(argv[1], "contradictory")) contradictory();
    else if (!strcmp(argv[1], "migration")) migration();
    else if (!strcmp(argv[1], "repair")) repair();
    else return 2;
    return 0;
}
