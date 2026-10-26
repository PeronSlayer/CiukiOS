/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include "users.h"
#include <ciuki/sha256.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int zeros(const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--) if (*b++) return 0;
    return 1;
}
static void rehash(unsigned char *b, size_t n)
{ memset(b + 64, 0, 32); sha256(b, n, b + 64); }
static void codec(const char *path)
{
    struct acct_db d, decoded;
    fake_seed(&d, 1);
    unsigned char b[ACCT_FILE_MAX + 1]; size_t n = 0;
    assert(!acct_encode(&d, 1, b, sizeof(b), &n) && n == 864);
    FILE *out = fopen(path, "wb"); assert(out);
    assert(fwrite(b, 1, n, out) == n && !fclose(out));
    assert(!acct_decode(&decoded, 1, b, n));
    assert(decoded.count == 3 && decoded.records[2].uid == 1000);
    assert(acct_decode(&decoded, 0, b, n) == -EIO);
    for (size_t i = 0; i < n; ++i) {
        b[i] ^= 1; memset(&decoded, 0x5a, sizeof(decoded));
        assert(acct_decode(&decoded, 1, b, n) == -EIO);
        assert(((unsigned char *)&decoded)[0] == 0x5a); b[i] ^= 1;
    }
    for (size_t i = 0; i < n; ++i) assert(acct_decode(&decoded, 1, b, i) == -EIO);
    assert(acct_decode(&decoded, 1, b, n + 1) == -EIO);
    static const unsigned offsets[] = {8, 12, 24, 28, 32, 36, 40, 52, 56, 60,
                                       96, 100, 104, 108, 112, 116, 121, 200, 296,
                                       352, 360, 608, 612, 616, 620, 624, 663};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        unsigned char saved = b[offsets[i]];
        b[offsets[i]] = offsets[i] == 24 ? 0 : 0xff; rehash(b, n);
        assert(acct_decode(&decoded, 1, b, n) == -EIO);
        b[offsets[i]] = saved; rehash(b, n);
    }
    struct acct_db bad = d;
    bad.records[2].iterations = 9999; assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; bad.next_uid = 1000; assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; strcpy(bad.records[2].name, "Guest"); assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; bad.records[2].uid = UINT32_MAX; assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; bad.sid[0] = 1; assert(acct_validate(&bad, 1) == -EINVAL);
    assert(acct_encode(&d, 1, b, n - 1, &n) == -ENOSPC);
    assert(acct_picture_syntax("catalogue/ciuki.png"));
    assert(!acct_picture_syntax("../secret") && !acct_picture_syntax("a/../b"));
    assert(!acct_picture_syntax("/System/foo") && !acct_picture_syntax("a//b"));
    puts("PASS codec: independent seed fixture, every truncated/corrupted byte, semantic corruption and unchanged failure output");
}
static void crypto(const char *hex)
{
    uint8_t salt[16], out[32], expected[32];
    assert(strlen(hex) == 64);
    for (unsigned i = 0; i < 16; ++i) salt[i] = (uint8_t)i;
    for (unsigned i = 0; i < 32; ++i) { unsigned x; assert(sscanf(hex + i * 2, "%2x", &x) == 1); expected[i] = (uint8_t)x; }
    assert(!acct_derive("password", salt, ACCT_ITERATIONS, out));
    assert(acct_equal_secret(out, expected, 32));
    for (unsigned i = 0; i < 32; ++i) { expected[i] ^= 1; assert(!acct_equal_secret(out, expected, 32)); expected[i] ^= 1; }
    assert(acct_derive("password", salt, 9999, out) == -EINVAL);
    assert(acct_derive("password", salt, UINT32_MAX, out) == -EINVAL);
    assert(acct_password_valid("12345678", 0) && acct_password_valid("        ", 0));
    assert(!acct_password_valid("1234567", 0) && !acct_password_valid("", 0));
    assert(!acct_password_valid("1234567\n", 0));
    char boundary[65]; memset(boundary, 'x', 63); boundary[63] = 0;
    assert(acct_password_valid(boundary, 0)); boundary[63] = 'x'; boundary[64] = 0;
    assert(!acct_password_valid(boundary, 0));
    assert(acct_reserved_name("sYsTeM") && acct_reserved_name("users"));
    assert(!acct_name_valid("a.b") && !acct_name_valid("a/b") && !acct_name_valid(""));
    assert(acct_name_valid("Alice_1-Test"));
    acct_wipe(boundary, sizeof(boundary)); assert(zeros(boundary, sizeof(boundary)));
    puts("PASS crypto: independent hashlib PBKDF2/10000 oracle, every verifier mismatch offset, boundaries and wiping");
}
static void service(void)
{
    struct fake f; fake_init(&f);
    struct acct_store store = fake_store(&f);
    struct acct_service s, other;
    assert(acct_service_open(&s, &store, 0) == -ENOSYS && !f.leased);
    f.deny_lease = 1; assert(acct_service_open(&s, &store, 1) == -EPERM); f.deny_lease = 0;
    f.deny_owner = 1; assert(acct_service_open(&s, &store, 1) == -EACCES && !f.leased); f.deny_owner = 0;
    fake_boot(&f, &s);
    assert(acct_service_open(&other, &store, 1) == -EBUSY);
    assert(s.db.sid[0] && s.db.records[0].kdf == 0 && s.db.records[1].kdf == 0);
    struct acct_request q; struct acct_response r;
    fake_request(&q, ACCT_LIST, 0, 0, NULL, NULL);
    assert(!fake_run(&s, 0, &q, &r) && r.count == 1 && r.records[0].uid == 1000);
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiUser", "");
    assert(!fake_run(&s, 0, &q, &r) && r.authenticated_uid == 1000);
    fake_request(&q, ACCT_AUTH, s.db.generation, 1000, "CiukiUser", "");
    assert(fake_run(&s, 2, &q, &r) == -EPERM);
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiAdministrator", "");
    assert(fake_run(&s, 0, &q, &r) == -EACCES && r.retry_after_ms == 1000);
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "unknown", "badpass1");
    assert(fake_run(&s, 0, &q, &r) == -EAGAIN);
    f.now += 999;
    assert(fake_run(&s, 0, &q, &r) == -EAGAIN && r.retry_after_ms == 1);
    ++f.now;
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "unknown", "badpass1");
    assert(fake_run(&s, 0, &q, &r) == -EACCES && r.retry_after_ms == 1000);
    f.now += 1000;
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD; q.flags = ACCT_ENABLED;
    assert(fake_run(&s, 2, &q, &r) == -EPERM);
    fake_request(&q, ACCT_CREATE, s.db.generation - 1, 0, "Bob", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -EAGAIN && s.db.next_uid == 1001);
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Guest", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -EINVAL);
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.flags = ACCT_ENABLED;
    assert(!fake_run(&s, 1, &q, &r) && s.db.count == 4 && s.db.next_uid == 1002);
    assert(s.db.records[3].groups == ACCT_STANDARD);
    uint64_t bob_generation = s.db.generation;
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "bob", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -EEXIST && s.db.generation == bob_generation);
    f.active_uid = 1001;
    const unsigned busy[] = {ACCT_DELETE, ACCT_ENABLE, ACCT_ROLE, ACCT_PASSWORD};
    for (unsigned i = 0; i < 4; ++i) {
        fake_request(&q, busy[i], s.db.generation, 1001, NULL, "password2"); q.groups = ACCT_ADMIN;
        assert(fake_run(&s, 1, &q, &r) == -EBUSY && s.db.generation == bob_generation);
    }
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 1001, NULL, "password2"); strcpy(q.old_password, "wrong");
    assert(fake_run(&s, 2, &q, &r) == -EACCES); f.now += 1000;
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 1001, NULL, "password2"); strcpy(q.old_password, "password1");
    assert(!fake_run(&s, 2, &q, &r));
    fake_request(&q, ACCT_PICTURE, s.db.generation, 1001, NULL, NULL); strcpy(q.picture, "catalogue/ciuki.png");
    assert(!fake_run(&s, 2, &q, &r));
    fake_request(&q, ACCT_PICTURE, s.db.generation, 1001, NULL, NULL); strcpy(q.picture, "catalogue/unapproved.png");
    assert(fake_run(&s, 2, &q, &r) == -EINVAL);
    f.active_uid = UINT32_MAX;
    fake_request(&q, ACCT_ROLE, s.db.generation, 1000, NULL, NULL); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) < 0); /* last normal administrator */
    fake_request(&q, ACCT_DELETE, s.db.generation, 1000, NULL, NULL);
    assert(fake_run(&s, 1, &q, &r) == -EPERM);
    fake_request(&q, ACCT_DELETE, s.db.generation, 500, NULL, NULL);
    assert(fake_run(&s, 1, &q, &r) == -EPERM);
    fake_request(&q, ACCT_ROLE, s.db.generation, 501, NULL, NULL); q.groups = ACCT_ADMIN;
    assert(fake_run(&s, 1, &q, &r) == -EPERM);
    fake_request(&q, ACCT_ENABLE, s.db.generation, 501, NULL, NULL); q.flags = ACCT_ENABLED;
    assert(fake_run(&s, 1, &q, &r) == -EINVAL);
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 501, NULL, "guestpass");
    assert(!fake_run(&s, 1, &q, &r));
    fake_request(&q, ACCT_ENABLE, s.db.generation, 501, NULL, NULL); q.flags = ACCT_ENABLED;
    assert(!fake_run(&s, 1, &q, &r));
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "Guest", "guestpass");
    assert(!fake_run(&s, 0, &q, &r) && r.authenticated_uid == 501 && s.db.records[1].groups == ACCT_GUEST);
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 501, NULL, "newguest"); strcpy(q.old_password, "wrong");
    assert(fake_run(&s, 3, &q, &r) == -EACCES); f.now += 1000;
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 501, NULL, "newguest"); strcpy(q.old_password, "guestpass");
    assert(!fake_run(&s, 3, &q, &r));
    fake_request(&q, ACCT_ENABLE, s.db.generation, 501, NULL, NULL);
    assert(!fake_run(&s, 1, &q, &r));
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 500, NULL, "recovery1");
    assert(!fake_run(&s, 1, &q, &r));
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiAdministrator", "recovery1");
    assert(fake_run(&s, 0, &q, &r) == -EACCES); f.now += 1000;
    f.peers[0].subject.safe_mode = 1;
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiAdministrator", "recovery1");
    assert(!fake_run(&s, 0, &q, &r) && r.authenticated_uid == 500);
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 1000, NULL, "newpass12");
    assert(!fake_run(&s, 1, &q, &r) && !s.db.auto_logon_uid && !(s.db.records[2].flags & ACCT_READY_BLANK));
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 1000, NULL, ""); strcpy(q.old_password, "newpass12");
    assert(fake_run(&s, 1, &q, &r) == -EINVAL);
    fake_request(&q, ACCT_RENAME, s.db.generation, 1001, "Robert", NULL);
    assert(!fake_run(&s, 1, &q, &r) && s.db.records[3].uid == 1001);
    fake_request(&q, ACCT_DELETE, s.db.generation, 1001, NULL, NULL);
    assert(!fake_run(&s, 1, &q, &r) && s.db.next_uid == 1002);
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Next", "password1"); q.groups = ACCT_STANDARD; q.flags = ACCT_ENABLED;
    assert(!fake_run(&s, 1, &q, &r) && s.db.records[3].uid == 1002);
    fake_request(&q, ACCT_LIST, 0, 0, NULL, NULL);
    assert(!fake_run(&s, 2, &q, &r) && r.count == 2); /* hidden built-ins filtered */
    acct_service_close(&s); assert(zeros(&s, sizeof(s)));
    fake_powercut(&f); assert(!acct_service_open(&s, &store, 1));
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiUser", "");
    assert(fake_run(&s, 0, &q, &r) == -EACCES); f.now += 1000;
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiUser", "newpass12");
    assert(!fake_run(&s, 0, &q, &r));
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS service: sealed/live refusal, singleton, trusted peers, auth/backoff, self/admin policy, Safe Mode, last-admin, active EBUSY, pictures, retirement and restart");
}

static void setup_seed(void)
{
    struct fake f; fake_init(&f); struct acct_service s; struct acct_store store = fake_store(&f);
    assert(!acct_service_open(&s, &store, 1));
    struct acct_db seed; fake_seed(&seed, 1); unsigned char b[ACCT_FILE_MAX]; size_t n;
    assert(!acct_encode(&seed, 1, b, sizeof(b), &n));
    unsigned events = f.event, entropy_calls = f.entropy_calls;
    assert(acct_service_bootstrap(&s, b, n, ACCT_SETUP) == -EINVAL);
    assert(f.event == events && f.entropy_calls == entropy_calls && !s.available);
    fake_seed(&seed, 0); assert(!acct_encode(&seed, 1, b, sizeof(b), &n));
    assert(!acct_service_bootstrap(&s, b, n, ACCT_SETUP));
    assert(s.db.origin == ACCT_SETUP && !s.db.auto_logon_uid && !strcmp(s.db.records[2].name, "Alice"));
    assert(!memcmp(s.db.records[0].verifier, seed.records[0].verifier, 32));
    assert(!memcmp(s.db.records[2].verifier, seed.records[2].verifier, 32));
    struct acct_request q; struct acct_response r;
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "Alice", "password");
    assert(!fake_run(&s, 0, &q, &r) && r.authenticated_uid == 1000);
    f.peers[0].subject.safe_mode = 1;
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiAdministrator", "password");
    assert(!fake_run(&s, 0, &q, &r) && r.authenticated_uid == 500);
    char root[64] = {0}; struct acct_owner_reply owner;
    assert(!store.owner.binding(store.owner.ctx, store.volume, 500, root, &owner));
    assert(owner.state == ACCT_OWNER_COMMITTED && !(s.db.records[0].flags & ACCT_ENABLED));
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS Setup seed: ready/Setup origin mismatch denied before work, verifiers preserved, custom uid 1000, configured disabled recovery with committed Safe Mode owner");
}

static void failure_inputs(void)
{
    for (unsigned failure = 1; failure <= 2; ++failure) {
        struct fake f; fake_init(&f); struct acct_service s;
        struct acct_store store = fake_store(&f);
        assert(!acct_service_open(&s, &store, 1));
        struct acct_db seed; fake_seed(&seed, 1); unsigned char b[ACCT_FILE_MAX]; size_t n;
        assert(!acct_encode(&seed, 1, b, sizeof(b), &n));
        f.fail_entropy = failure;
        assert(acct_service_bootstrap(&s, b, n, ACCT_READY) == -EAGAIN && !s.available);
        assert(fake_file_index(&f, "accounts.db") < 0 && fake_file_index(&f, "provision.pending") < 0);
        acct_service_close(&s); fake_destroy(&f);
    }
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
    struct acct_request q; struct acct_response r;
    f.fail_entropy = f.entropy_calls + 1;
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -EAGAIN && s.db.next_uid == 1001);
    f.fail_alloc = f.allocations + 1;
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -ENOMEM && s.db.next_uid == 1001);
    f.owner_full = 1;
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -ENOSPC && s.db.next_uid == 1002 && !s.available);
    acct_service_close(&s); f.owner_full = 0;
    struct acct_store store = fake_store(&f);
    assert(!acct_service_open(&s, &store, 1) && s.db.next_uid == 1002 && s.db.count == 4);
    acct_service_close(&s);
    /* Store loss after provisioning cannot replay the seed or use a stale lock. */
    int i = fake_file_index(&f, "accounts.db"); assert(i >= 0);
    f.files[i].name[0] = f.files[i].durable_name[0] = 0;
    fake_put(&f, "users.lock", "stale", 5);
    assert(acct_service_open(&s, &store, 1) == -EIO && !f.leased);
    fake_destroy(&f);
    puts("PASS failures: bootstrap/salt entropy, OOM wiping, owner ENOSPC reservation, recovery and consumed-seed refusal");
}

static void capacity(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
    struct acct_request q; struct acct_response r;
    for (unsigned i = 0; i < 15; ++i) {
        char name[32]; snprintf(name, sizeof(name), "User%u", i);
        fake_request(&q, ACCT_CREATE, s.db.generation, 0, name, "password1"); q.groups = ACCT_STANDARD;
        assert(!fake_run(&s, 1, &q, &r));
    }
    assert(s.db.count == 18 && s.db.next_uid == 1016);
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Overflow", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -ENOSPC && s.db.next_uid == 1016);
    fake_request(&q, ACCT_DELETE, s.db.generation, 1001, NULL, NULL);
    assert(!fake_run(&s, 1, &q, &r));
    struct acct_db next = s.db; ++next.generation; next.next_uid = ACCT_LAST_UID;
    assert(!acct_store_commit(&s.store, &s.db, &next, NULL, 0)); s.db = next;
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "FinalUser", "password1"); q.groups = ACCT_STANDARD;
    assert(!fake_run(&s, 1, &q, &r));
    assert(s.db.records[17].uid == ACCT_LAST_UID && s.db.next_uid == UINT32_MAX && (s.db.flags & ACCT_EXHAUSTED));
    fake_request(&q, ACCT_DELETE, s.db.generation, 1002, NULL, NULL);
    assert(!fake_run(&s, 1, &q, &r));
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "NeverReuse", "password1"); q.groups = ACCT_STANDARD;
    assert(fake_run(&s, 1, &q, &r) == -ENOSPC);
    next = s.db; next.generation = UINT64_MAX;
    unsigned char b[ACCT_FILE_MAX]; size_t n;
    assert(!acct_encode(&next, 0, b, sizeof(b), &n));
    acct_service_close(&s); fake_put(&f, "accounts.db", b, n);
    struct acct_store store = fake_store(&f); assert(!acct_service_open(&s, &store, 1));
    fake_request(&q, ACCT_PASSWORD, s.db.generation, 1000, NULL, "newpass12");
    assert(fake_run(&s, 1, &q, &r) == -EOVERFLOW);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS capacity: 18-account limit, durable high-water, final legal uid, no sentinel/reuse, generation exhaustion");
}

static void disk_oom_corruption(void)
{
    for (unsigned failure = 1; failure <= 3; ++failure) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        f.fail_alloc = f.allocations + failure;
        struct acct_request q; struct acct_response r;
        fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD;
        assert(fake_run(&s, 1, &q, &r) == -ENOMEM);
        assert(s.db.next_uid == (failure == 3 ? 1002u : 1001u));
        acct_service_close(&s);
        struct acct_store store = fake_store(&f); assert(!acct_service_open(&s, &store, 1));
        assert(s.db.count == 3 && s.db.next_uid == (failure == 3 ? 1002u : 1001u));
        acct_service_close(&s); fake_destroy(&f);
    }
    for (unsigned failure = 1; failure <= 3; ++failure) {
        struct fake f; fake_init(&f); struct acct_service s; struct acct_store store = fake_store(&f);
        assert(!acct_service_open(&s, &store, 1));
        struct acct_db seed; fake_seed(&seed, 1); unsigned char b[ACCT_FILE_MAX]; size_t n;
        assert(!acct_encode(&seed, 1, b, sizeof(b), &n));
        f.fail_alloc = f.allocations + failure;
        assert(acct_service_bootstrap(&s, b, n, ACCT_READY) == -ENOMEM && !s.available);
        acct_service_close(&s); assert(!acct_service_open(&s, &store, 1));
        assert(s.available == (failure == 3));
        acct_service_close(&s); fake_destroy(&f);
    }
    static const char *const write_names[] = {"accounts.db.new", "accounts.db.txn"};
    for (unsigned failure = 0; failure < 2; ++failure) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        f.write_error = ENOSPC; strcpy(f.write_error_name, write_names[failure]);
        struct acct_request q; struct acct_response r;
        fake_request(&q, ACCT_PASSWORD, s.db.generation, 1000, NULL, "newpass12");
        assert(fake_run(&s, 1, &q, &r) == -ENOSPC && !s.available);
        acct_service_close(&s); f.write_error = 0; fake_powercut(&f);
        struct acct_store store = fake_store(&f); assert(!acct_service_open(&s, &store, 1));
        assert(s.db.generation == 1 && s.db.auto_logon_uid == 1000);
        acct_service_close(&s); fake_destroy(&f);
    }
    for (unsigned failure = 0; failure < 10; ++failure) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        unsigned char b[ACCT_FILE_MAX]; size_t n;
        assert(!acct_encode(&s.db, 0, b, sizeof(b), &n)); acct_service_close(&s);
        int i = fake_file_index(&f, "accounts.db"); assert(i >= 0);
        if (failure == 0) { b[300] ^= 1; fake_put(&f, "accounts.db", b, n); }
        else if (failure == 1) fake_put(&f, "accounts.db", b, n - 1);
        else if (failure == 2) fake_put(&f, "accounts.db.txn", "torn", 4);
        else if (failure == 3) fake_put(&f, "accounts.db.bak", b, n);
        else if (failure == 4) fake_put(&f, "provision.used", "torn", 4);
        else if (failure == 5) f.wrong_owner = 1;
        else if (failure == 6) f.owner_used = 0;
        else if (failure == 7) {
            f.files[i].name[0] = f.files[i].durable_name[0] = 0;
            int marker = fake_file_index(&f, "provision.used"); assert(marker >= 0);
            f.files[marker].name[0] = f.files[marker].durable_name[0] = 0;
        }
        else if (failure == 8) {
            strcpy((char *)b + 96 + 256 * 2 + 104, "catalogue/unapproved.png");
            rehash(b, n); fake_put(&f, "accounts.db", b, n);
        } else strcpy(f.owners[0].request.root, "/Windows/Profiles/../System");
        struct acct_store store = fake_store(&f);
        assert(acct_service_open(&s, &store, 1) == -EIO && !f.leased);
        fake_destroy(&f);
    }
    puts("PASS storage failures: every bootstrap/create allocation, partial ENOSPC writes, corrupt/truncated database/intent/marker, stale backup and missing ownership/provision evidence");
}

struct submit_context { struct acct_service *s; pthread_barrier_t *barrier; int result; };
static void *submit_thread(void *arg)
{
    struct submit_context *ctx = arg;
    struct acct_request q; struct acct_response r;
    fake_request(&q, ACCT_AUTH, ctx->s->db.generation, 0, "CiukiUser", "");
    int b = pthread_barrier_wait(ctx->barrier); assert(!b || b == PTHREAD_BARRIER_SERIAL_THREAD);
    ctx->result = acct_submit(ctx->s, 0, &q, sizeof(q), &r);
    assert(zeros(q.password, sizeof(q.password)) && zeros(q.confirm, sizeof(q.confirm)));
    return NULL;
}
static void concurrency(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
    pthread_t threads[16]; struct submit_context contexts[16]; pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, 16));
    for (unsigned i = 0; i < 16; ++i) {
        contexts[i] = (struct submit_context){&s, &barrier, 0};
        assert(!pthread_create(&threads[i], NULL, submit_thread, &contexts[i]));
    }
    unsigned accepted = 0;
    for (unsigned i = 0; i < 16; ++i) {
        assert(!pthread_join(threads[i], NULL));
        assert(contexts[i].result == 0 || contexts[i].result == -EAGAIN);
        if (!contexts[i].result) ++accepted;
    }
    assert(accepted >= 1 && accepted <= 2);
    struct acct_response r; for (unsigned i = 0; i < accepted; ++i) assert(acct_work(&s, &r) == 1 && !r.error);
    assert(!acct_work(&s, &r));
    assert(!pthread_barrier_destroy(&barrier));
    struct acct_request q;
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiUser", "wrong");
    assert(!acct_submit(&s, 0, &q, sizeof(q), &r));
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiUser", "");
    assert(!acct_submit(&s, 0, &q, sizeof(q), &r));
    assert(acct_work(&s, &r) == 1 && r.error == -EACCES);
    assert(acct_work(&s, &r) == -EAGAIN && r.retry_after_ms == 1000);
    f.now += 1000; assert(acct_work(&s, &r) == 1 && !r.error);
    fake_request(&q, ACCT_AUTH, s.db.generation, 0, "CiukiUser", "");
    assert(!acct_submit(&s, 0, &q, sizeof(q), &r));
    ++f.peers[0].epoch; /* same fd/PID, different endpoint generation */
    assert(acct_work(&s, &r) == 1 && r.error == -ESRCH && !r.authenticated_uid);
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD;
    f.cancel_on_prepare = 1;
    assert(fake_run(&s, 1, &q, &r) == -ESRCH && f.abort_calls == 1 && s.db.next_uid == 1002);
    assert(fake_file_index(&f, "accounts.db.txn") < 0);
    uint64_t cancelled_generation = s.db.generation + 1;
    acct_service_close(&s);
    f.cancel_on_prepare = 0; f.peers[1].alive = 1;
    struct acct_store store = fake_store(&f);
    assert(!acct_service_open(&s, &store, 1) && s.db.generation == cancelled_generation);
    fake_request(&q, ACCT_CREATE, s.db.generation, 0, "AfterCancel", "password1"); q.groups = ACCT_STANDARD;
    assert(!fake_run(&s, 1, &q, &r) && s.db.records[3].uid == 1002);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS concurrency: 16 clients, bounded two-slot admission, pending retry delay, retained-link reuse cancellation and pending owner ABORT");
}

static void faults(void)
{
    unsigned password_events = 0, bootstrap_events = 0, create_events = 0, delete_events = 0, cancel_events = 0;
    /* Every persistence boundary, including owner PREPARE/COMMIT, namespace
     * operations and cleanup. A second pass enumerates each observed event. */
    for (unsigned run = 0; run <= password_events; ++run) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        struct acct_db next = s.db; ++next.generation; next.auto_logon_uid = 0;
        next.records[2].flags &= ~ACCT_READY_BLANK;
        assert(!acct_derive("newpass12", next.records[2].salt, ACCT_ITERATIONS, next.records[2].verifier));
        f.event = 0; f.cut = run;
        int e = acct_store_commit(&s.store, &s.db, &next, NULL, 0);
        if (!run) { assert(!e); password_events = f.event; }
        else assert(e == -EIO);
        acct_service_close(&s); fake_powercut(&f);
        struct acct_store store = fake_store(&f);
        e = acct_service_open(&s, &store, 1);
        assert(!e); /* all complete-write/flush cuts have a valid old/new state */
        assert(s.db.generation == 1 || s.db.generation == 2);
        if (!run) assert(s.db.generation == 2); /* acknowledged reset never rolls back */
        if (s.db.generation == 2) assert(!memcmp(s.db.records[2].verifier, next.records[2].verifier, 32));
        else assert(s.db.auto_logon_uid == 1000);
        acct_service_close(&s); fake_destroy(&f);
    }
    for (unsigned run = 0; run <= bootstrap_events; ++run) {
        struct fake f; fake_init(&f); struct acct_service s; struct acct_store store = fake_store(&f);
        assert(!acct_service_open(&s, &store, 1));
        struct acct_db seed; fake_seed(&seed, 1); unsigned char b[ACCT_FILE_MAX]; size_t n;
        assert(!acct_encode(&seed, 1, b, sizeof(b), &n));
        f.event = 0; f.cut = run;
        int e = acct_service_bootstrap(&s, b, n, ACCT_READY);
        if (!run) { assert(!e); bootstrap_events = f.event; }
        else assert(e == -EIO);
        uint32_t sid[3]; memcpy(sid, s.db.sid, sizeof(sid));
        acct_service_close(&s); fake_powercut(&f);
        e = acct_service_open(&s, &store, 1);
        assert(!e);
        if (s.available) {
            assert(s.db.count == 3 && s.db.sid[0]);
            if (!run) assert(!memcmp(sid, s.db.sid, sizeof(sid)));
            assert(f.commit_calls >= 1);
        } else assert(f.owner_used == 0); /* cut before pending database is durable */
        acct_service_close(&s); fake_destroy(&f);
    }
    for (unsigned run = 0; run <= create_events; ++run) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        struct acct_request q; struct acct_response r;
        fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD; q.flags = ACCT_ENABLED;
        f.event = 0; f.cut = run;
        int e = fake_run(&s, 1, &q, &r);
        if (!run) { assert(!e); create_events = f.event; }
        else assert(e == -EIO);
        acct_service_close(&s); fake_powercut(&f);
        struct acct_store store = fake_store(&f); assert(!acct_service_open(&s, &store, 1));
        if (s.db.count == 4) assert(s.db.next_uid == 1002 && s.db.records[3].uid == 1001);
        else assert(s.db.count == 3 && (s.db.next_uid == 1001 || s.db.next_uid == 1002));
        if (!run) assert(s.db.count == 4);
        if (f.owner_used > 1) assert(s.db.next_uid == 1002); /* owner publication implies durable uid reservation */
        acct_service_close(&s); fake_destroy(&f);
    }
    for (unsigned run = 0; run <= delete_events; ++run) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        struct acct_request q; struct acct_response r;
        fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD; q.flags = ACCT_ENABLED;
        assert(!fake_run(&s, 1, &q, &r));
        fake_request(&q, ACCT_DELETE, s.db.generation, 1001, NULL, NULL);
        f.event = 0; f.cut = run;
        int e = fake_run(&s, 1, &q, &r);
        if (!run) { assert(!e); delete_events = f.event; }
        else assert(e == -EIO);
        acct_service_close(&s); fake_powercut(&f);
        struct acct_store store = fake_store(&f); assert(!acct_service_open(&s, &store, 1));
        assert(s.db.next_uid == 1002 && (s.db.count == 3 || s.db.count == 4));
        if (!run) assert(s.db.count == 3);
        acct_service_close(&s); fake_destroy(&f);
    }
    for (unsigned run = 0; run <= cancel_events; ++run) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f, &s);
        struct acct_request q; struct acct_response r;
        fake_request(&q, ACCT_CREATE, s.db.generation, 0, "Bob", "password1"); q.groups = ACCT_STANDARD; q.flags = ACCT_ENABLED;
        f.cancel_on_prepare = 1; f.event = 0; f.cut = run;
        int e = fake_run(&s, 1, &q, &r);
        if (!run) { assert(e == -ESRCH); cancel_events = f.event; }
        else assert(e == -EIO || e == -ESRCH);
        acct_service_close(&s); fake_powercut(&f); f.cancel_on_prepare = 0;
        struct acct_store store = fake_store(&f); assert(!acct_service_open(&s, &store, 1));
        if (f.abort_calls) assert(s.db.count == 3 && s.db.next_uid == 1002 && s.db.generation >= 3);
        acct_service_close(&s); fake_destroy(&f);
    }
    printf("PASS power cuts: every %u password, %u bootstrap, %u creation, %u deletion and %u cancellation persistence/owner boundary; acknowledged password retained\n",
           password_events, bootstrap_events, create_events, delete_events, cancel_events);
}

static void migration(void)
{
    struct users_db old = {0}; old.count = 3; old.next_id = 20;
    const uint32_t ids[3] = {9, 2, 5};
    const char *const names[3] = {"SYSTEM", "Short", "Empty"};
    const char *const passwords[3] = {"password", "abc", ""};
    for (unsigned i = 0; i < 3; ++i) {
        struct users_record *r = &old.records[i]; r->id = ids[i]; strcpy(r->name, names[i]);
        memset(r->salt, (int)i, sizeof(r->salt)); users_derive(passwords[i], r->salt, USERS_ITERATIONS, r->verifier);
    }
    unsigned char b[1400]; size_t n = users_encode(&old, b, sizeof(b)); assert(n);
    struct acct_import_options options = {0}; options.first_legacy_id = 9;
    struct acct_db imported; struct acct_import_map maps[16]; unsigned count = 0;
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EINVAL);
    strcpy(options.replacement_names[2], "FirstAdmin");
    assert(!acct_import_g32(b, n, &options, &imported, maps, &count) && count == 3);
    assert(imported.records[2].uid == 1000 && imported.records[2].legacy_id == 9 && imported.records[2].groups == ACCT_ADMIN);
    assert(imported.records[3].uid == 1001 && imported.records[3].legacy_id == 2 && imported.records[3].groups == ACCT_STANDARD);
    assert(imported.records[3].flags & ACCT_CHANGE_REQUIRED);
    assert(imported.records[4].legacy_id == 5 && !(imported.records[4].flags & ACCT_ENABLED));
    assert(maps[0].legacy_id == 2 && maps[0].uid == 1001 && !strcmp(maps[0].root, "/Windows/Profiles/u00000002"));
    assert(maps[2].uid == 1000 && !memcmp(imported.records[2].verifier, old.records[0].verifier, 32));
    assert(acct_validate(&imported, 0) == -EINVAL); /* no SID/enrollment, never publish draft */
    options.first_legacy_id = 1;
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EINVAL);
    options.first_legacy_id = 9; strcpy(options.replacement_names[0], "FirstAdmin");
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EEXIST);
    options.replacement_names[0][0] = 0;
    b[70] ^= 1; assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EIO); b[70] ^= 1;
    assert(acct_import_g32(b, n - 1, &options, &imported, maps, &count) == -EIO);
    old.count = 0; n = users_encode(&old, b, sizeof(b));
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -ENODATA);
    old.count = 16; old.next_id = 1001;
    for (unsigned i = 0; i < 16; ++i) {
        old.records[i] = old.records[0]; old.records[i].id = i == 15 ? 1000 : 1 + i;
        snprintf(old.records[i].name, 32, "Legacy%u", i);
    }
    n = users_encode(&old, b, sizeof(b)); assert(n == sizeof(b));
    memset(&options, 0, sizeof(options)); options.first_legacy_id = 2;
    assert(!acct_import_g32(b, n, &options, &imported, maps, &count) && count == 16 && imported.count == 18);
    assert(imported.records[2].legacy_id == 2 && imported.records[2].uid == 1000);
    assert(maps[15].legacy_id == 1000 && maps[15].uid != 1000 && !strcmp(maps[15].root, "/Windows/Profiles/u000003e8"));
    assert(imported.next_uid == 1016);
    puts("PASS G32 import: explicit administrator, sorted monotonic mapping, preserved verifiers/roots, reserved-name enrollment, short/blank and corrupt/empty sources");
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc >= 2);
    if (!strcmp(argv[1], "crypto")) { assert(argc == 3); crypto(argv[2]); }
    else if (!strcmp(argv[1], "codec")) { assert(argc == 3); codec(argv[2]); }
    else if (!strcmp(argv[1], "service")) service();
    else if (!strcmp(argv[1], "setup-seed")) setup_seed();
    else if (!strcmp(argv[1], "failures")) failure_inputs();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "disk-oom")) disk_oom_corruption();
    else if (!strcmp(argv[1], "concurrency")) concurrency();
    else if (!strcmp(argv[1], "faults")) faults();
    else if (!strcmp(argv[1], "migration")) migration();
    else assert(0);
    return 0;
}
