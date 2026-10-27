/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <string.h>

static unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int acct_name_equal(const char *a, const char *b)
{
    for (unsigned i = 0; i < 32; ++i) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}
int acct_name_valid(const char *s)
{
    if (!s || !s[0]) return 0;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned c = lower((unsigned char)s[i]);
        if (!c) return 1;
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '-') return 0;
    }
    return 0;
}
int acct_reserved_name(const char *s)
{
    static const char *const names[] = {"SYSTEM", "CiukiAdministrator", "CiukiUser",
                                        "Guest", "Administrators", "Users", "Guests"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (acct_name_equal(s, names[i])) return 1;
    return 0;
}
int acct_picture_syntax(const char *s)
{
    if (!s || !memchr(s, 0, 96)) return 0;
    if (!*s) return 1;
    if (s[0] == '/') return 0;
    unsigned component = 0;
    for (unsigned i = 0; s[i]; ++i) {
        unsigned c = (unsigned char)s[i];
        if (c == '/') {
            if (!component) return 0;
            component = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
            if (c == '.' && (!component || s[i - 1] == '.')) return 0;
            ++component;
        }
    }
    return component != 0;
}
int acct_profile_root_valid(const char root[64])
{
    static const char prefix[] = "/Windows/Profiles/u";
    const size_t start = sizeof(prefix) - 1;
    if (memcmp(root, prefix, start)) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        char c = root[start + i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return 0;
    }
    for (size_t i = start + 8; i < 64; ++i) if (root[i]) return 0;
    return 1;
}
static int zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    unsigned bits = 0;
    while (n--) bits |= *b++;
    return bits == 0;
}
static int padded(const char *s, size_t n)
{
    const char *end = memchr(s, 0, n);
    return end && zero(end, n - (size_t)(end - s));
}

int acct_validate(const struct acct_db *d, int seed)
{
    if (!d || (seed ? d->generation != 0 : !d->generation) || d->count < 2 || d->count > ACCT_MAX ||
        (d->flags & ~7u) || d->origin < ACCT_READY || d->origin > ACCT_UPGRADE ||
        (seed && d->origin == ACCT_UPGRADE) ||
        (seed ? !zero(d->sid, sizeof(d->sid)) : zero(d->sid, sizeof(d->sid))) ||
        (d->next_uid < 1000) ||
        (!!(d->flags & ACCT_EXHAUSTED) != (d->next_uid == UINT32_MAX)) ||
        (d->origin == ACCT_UPGRADE && !(d->flags & ACCT_MIGRATED)) ||
        (d->origin != ACCT_UPGRADE && (d->flags & ACCT_MIGRATED)) ||
        (d->auto_logon_uid && (d->auto_logon_uid != 1000 || d->origin != ACCT_READY))) return -EINVAL;
    int ready = 0, normal_admin = 0;
    unsigned enabled = 0;
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        if ((i && d->records[i - 1].uid >= r->uid) || (r->flags & ~31u) ||
            !acct_name_valid(r->name) || !padded(r->name, 32) ||
            !acct_picture_syntax(r->picture) || !padded(r->picture, 96)) return -EINVAL;
        if (i == 0) {
            if (r->uid != 500 || strcmp(r->name, "CiukiAdministrator") ||
                r->groups != ACCT_ADMIN || !(r->flags & ACCT_BUILTIN)) return -EINVAL;
        } else if (i == 1) {
            if (r->uid != 501 || strcmp(r->name, "Guest") ||
                r->groups != ACCT_GUEST || !(r->flags & ACCT_BUILTIN)) return -EINVAL;
        } else {
            if (r->uid < 1000 || r->uid >= d->next_uid || (r->flags & ACCT_BUILTIN) ||
                (r->groups != ACCT_ADMIN && r->groups != ACCT_STANDARD) ||
                (acct_reserved_name(r->name) && !(d->origin == ACCT_READY && r->uid == 1000 &&
                                                       !strcmp(r->name, "CiukiUser")))) return -EINVAL;
        }
        if (r->legacy_id && (d->origin != ACCT_UPGRADE || r->uid < 1000)) return -EINVAL;
        if ((r->flags & ACCT_CHANGE_REQUIRED) &&
            (!r->legacy_id || (r->flags & ACCT_ENABLED) || !(r->flags & ACCT_PASSWORD_SET))) return -EINVAL;
        enabled += !!(r->flags & ACCT_ENABLED);
        for (unsigned j = 0; j < i; ++j)
            if (acct_name_equal(r->name, d->records[j].name) ||
                (r->legacy_id && r->legacy_id == d->records[j].legacy_id)) return -EINVAL;
        if (r->flags & ACCT_READY_BLANK) {
            if (r->uid != 1000 || d->origin != ACCT_READY || strcmp(r->name, "CiukiUser") ||
                r->groups != ACCT_ADMIN ||
                (r->flags & ACCT_CHANGE_REQUIRED) || r->legacy_id ||
                !(r->flags & ACCT_ENABLED) || d->auto_logon_uid != 1000 ||
                (d->flags & ACCT_AUTO_RETIRED)) return -EINVAL;
            ready = !!(r->flags & ACCT_ENABLED);
        }
        if (r->flags & ACCT_PASSWORD_SET) {
            if (r->kdf != 1 || r->iterations != ACCT_ITERATIONS) return -EINVAL;
        } else if (r->kdf || r->iterations || !zero(r->salt, 16) || !zero(r->verifier, 32) ||
                   ((r->flags & ACCT_ENABLED) && !(seed && (r->flags & ACCT_READY_BLANK)))) return -EINVAL;
        if ((r->flags & ACCT_READY_BLANK) && !seed && !(r->flags & ACCT_PASSWORD_SET)) return -EINVAL;
        if (seed && (r->flags & ACCT_READY_BLANK) && (r->flags & ACCT_PASSWORD_SET)) return -EINVAL;
        if (r->groups == ACCT_ADMIN && (r->flags & ACCT_ENABLED) &&
            !(r->flags & ACCT_CHANGE_REQUIRED) &&
            ((r->flags & ACCT_PASSWORD_SET) || (seed && (r->flags & ACCT_READY_BLANK)))) normal_admin = 1;
    }
    if (!normal_admin) return -EINVAL;
    if ((ready && enabled != 1) || (d->auto_logon_uid && !ready) ||
        ((d->flags & ACCT_AUTO_RETIRED) && d->auto_logon_uid)) return -EINVAL;
    if (seed && (d->flags || d->next_uid != 1001 || d->count != 3 || d->records[2].uid != 1000 ||
                 d->records[2].groups != ACCT_ADMIN || !(d->records[2].flags & ACCT_ENABLED) ||
                 (d->records[0].flags & ACCT_ENABLED) || (d->records[1].flags & ACCT_ENABLED) ||
                 (d->origin == ACCT_READY && (!ready || d->auto_logon_uid != 1000 || (d->records[0].flags & ACCT_PASSWORD_SET))) ||
                 (d->origin == ACCT_SETUP && (ready || d->auto_logon_uid || !(d->records[0].flags & ACCT_PASSWORD_SET))))) return -EINVAL;
    if (seed) {
        const struct acct_record *a = d->records;
        if (a[0].flags != (d->origin == ACCT_READY ? 2u : 6u) || a[1].flags != 2 ||
            a[2].flags != (d->origin == ACCT_READY ? 9u : 5u)) return -EINVAL;
        for (unsigned i = 0; i < 3; ++i)
            if (a[i].legacy_id || a[i].picture[0]) return -EINVAL;
        if (d->origin == ACCT_SETUP && !memcmp(a[0].salt, a[2].salt, 16)) return -EINVAL;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t x)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (8 * i)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put64(uint8_t *p, uint64_t x)
{ put32(p, (uint32_t)x); put32(p + 4, (uint32_t)(x >> 32)); }
static uint64_t get64(const uint8_t *p)
{ return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void digest(const uint8_t *b, size_t n, uint8_t out[32])
{
    const uint8_t zeros[32] = {0};
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, b, 64);
    sha256_update(&ctx, zeros, 32);
    sha256_update(&ctx, b + 96, n - 96);
    sha256_final(&ctx, out);
    acct_wipe(&ctx, sizeof(ctx));
}
int acct_encode(const struct acct_db *d, int seed, void *buffer, size_t cap, size_t *length)
{
    int e = acct_validate(d, seed);
    if (e || !buffer || !length) return e ? e : -EINVAL;
    size_t n = 96 + 256 * d->count;
    if (cap < n) return -ENOSPC;
    uint8_t *b = buffer;
    memset(b, 0, n);
    memcpy(b, seed ? "CIUKISEE" : "CIUKISEC", 8);
    put32(b + 8, 2); put32(b + 12, 96); put64(b + 16, d->generation);
    put32(b + 24, d->next_uid); put32(b + 28, d->count); put32(b + 32, d->flags);
    put32(b + 36, 256);
    for (unsigned i = 0; i < 3; ++i) put32(b + 40 + i * 4, d->sid[i]);
    put32(b + 52, d->origin); put32(b + 56, d->auto_logon_uid);
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        uint8_t *p = b + 96 + 256 * i;
        put32(p, r->uid); put32(p + 4, r->flags); put32(p + 8, r->groups);
        put32(p + 12, r->kdf); put32(p + 16, r->iterations); put32(p + 20, r->legacy_id);
        memcpy(p + 24, r->name, 32); memcpy(p + 56, r->salt, 16);
        memcpy(p + 72, r->verifier, 32); memcpy(p + 104, r->picture, 96);
    }
    digest(b, n, b + 64);
    *length = n;
    return 0;
}
int acct_decode(struct acct_db *d, int seed, const void *buffer, size_t n)
{
    const uint8_t *b = buffer;
    if (!d || !b || n < 96 || n > ACCT_FILE_MAX ||
        memcmp(b, seed ? "CIUKISEE" : "CIUKISEC", 8) || get32(b + 8) != 2 ||
        get32(b + 12) != 96 || get32(b + 36) != 256 || get32(b + 28) > ACCT_MAX ||
        n != 96 + 256 * get32(b + 28) || get32(b + 60)) return -EIO;
    uint8_t sum[32];
    digest(b, n, sum);
    int valid = acct_equal_secret(sum, b + 64, 32);
    acct_wipe(sum, sizeof(sum));
    if (!valid) return -EIO;
    struct acct_db tmp = {0};
    tmp.generation = get64(b + 16); tmp.next_uid = get32(b + 24);
    tmp.count = get32(b + 28); tmp.flags = get32(b + 32);
    for (unsigned i = 0; i < 3; ++i) tmp.sid[i] = get32(b + 40 + i * 4);
    tmp.origin = get32(b + 52); tmp.auto_logon_uid = get32(b + 56);
    for (unsigned i = 0; i < tmp.count; ++i) {
        struct acct_record *r = &tmp.records[i];
        const uint8_t *p = b + 96 + 256 * i;
        if (!zero(p + 200, 56)) { acct_wipe(&tmp, sizeof(tmp)); return -EIO; }
        r->uid = get32(p); r->flags = get32(p + 4); r->groups = get32(p + 8);
        r->kdf = get32(p + 12); r->iterations = get32(p + 16); r->legacy_id = get32(p + 20);
        memcpy(r->name, p + 24, 32); memcpy(r->salt, p + 56, 16);
        memcpy(r->verifier, p + 72, 32); memcpy(r->picture, p + 104, 96);
    }
    int e = acct_validate(&tmp, seed);
    if (!e) *d = tmp;
    acct_wipe(&tmp, sizeof(tmp));
    return e ? -EIO : 0;
}
