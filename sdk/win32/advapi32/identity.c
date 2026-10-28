/* sec-user-05: the read-only Win32 identity facade (user-security-contract.md
 * section 8.1; user-security-kernel.md "Read-only token/SID facade and
 * privilege view"). Every answer is built in caller memory from three
 * authoritative sources and nothing here grants a right:
 *
 *  - the kernel's retained credential, read through a closeable, noninheritable
 *    W32_OBJ_TOKEN query handle (sec-k06; query access only);
 *  - the protected per-install machine SID components A/B/C, from the
 *    REGISTRY broker's RB_MACHINE_ID over ntdll's existing link;
 *  - filtered ACCOUNTS LOOKUP records (public uid, groups and name only).
 *
 * Local accounts are S-1-5-21-A-B-C-uid, SYSTEM is S-1-5-18 and the built-in
 * groups are S-1-5-32-544/545/546. Privileges are the frozen three-entry view
 * (SeShutdownPrivilege=1, SeSystemtimePrivilege=2, SeLoadDriverPrivilege=3,
 * attributes 3, granted entries only). SIDs that a program builds in its own
 * memory are data: membership answers come from the kernel credential, and
 * the kernel and brokers make every authorization decision. No impersonation,
 * token duplication/assignment, privilege adjustment or network lookup exists;
 * those entry points keep their typed refusals.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "internal.h"
#include <ciuki/abi.h>
#include <ciuki/w32_transport.h>
#ifdef W32_HOST_MODEL
#define IMPORT
#else
#define IMPORT __declspec(dllimport)
#endif
IMPORT int W32API GetComputerNameA(char *, uint32_t *);
extern uint32_t w32r_ansi_scalar(uint8_t);

#define TOKEN_QUERY 0x8u
#define TOKEN_QUERY_SOURCE 0x10u
#define READ_CONTROL 0x20000u
#define MAXIMUM_ALLOWED 0x02000000u
#define GENERIC_READ 0x80000000u
/* Rights that only read the token. Duplicate/impersonate/assign/adjust and
 * write rights are refused (ERROR_ACCESS_DENIED) before any handle exists. */
#define QUERY_RIGHTS (TOKEN_QUERY | TOKEN_QUERY_SOURCE | READ_CONTROL | MAXIMUM_ALLOWED | GENERIC_READ)
enum { TokenUser = 1, TokenGroups, TokenPrivileges, TokenOwner, TokenPrimaryGroup,
       TokenType = 8, TokenSessionId = 12 };
enum { SidTypeUser = 1, SidTypeDomain = 3, SidTypeAlias = 4, SidTypeWellKnownGroup = 5 };
#define SID_BYTES 28u /* revision, count, authority[6], at most five subauthorities here */

static int fail(uint32_t e)
{
    w32r_last_error(e);
    return 0;
}
/* No C runtime in this DLL: byte comparison and bounded text copies without
 * memcmp/ntdll string imports. */
static void copy_text(char *out, uint32_t cap, const char *s)
{
    uint32_t n = w32_strlen(s);
    if (n >= cap)
        n = cap - 1;
    w32_copy(out, s, n);
    out[n] = 0;
}
static int same_bytes(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;
    for (uint32_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}
static uint32_t sid_length(const void *sid)
{
    const uint8_t *s = sid;
    return s && s[0] == 1 && s[1] <= 15 ? 8u + 4u * s[1] : 0;
}
static uint32_t sid_make(uint8_t *out, uint8_t authority, uint32_t count, const uint32_t *sub)
{
    w32_fill(out, 0, 8);
    out[0] = 1;
    out[1] = (uint8_t)count;
    out[7] = authority;
    for (uint32_t i = 0; i < count; i++)
        w32_put32(out + 8 + 4 * i, sub[i]);
    return 8 + 4 * count;
}
static uint32_t sid_builtin(uint8_t *out, uint32_t rid)
{
    const uint32_t sub[2] = {32, rid};
    return sid_make(out, 5, 2, sub);
}
static uint32_t sid_system(uint8_t *out)
{
    const uint32_t sub[1] = {18};
    return sid_make(out, 5, 1, sub);
}

/* The kernel credential behind a query handle. */
struct identity {
    struct ciuki_credential c;
    uint32_t privileges; /* kernel bits: shutdown 1, system time 2, managed driver 4 */
};
static uint32_t query(uint32_t handle, struct identity *out)
{
    struct ciuki_w32_object_args a = {.action = W32_OBJ_QUERY, .type = W32_OBJ_TOKEN, .handle = handle,
                                      .maximum = sizeof(out->c)};
    uint32_t e = w32r_object(&a, &out->c);
    if (e)
        return e;
    if (a.result != sizeof(out->c) || a.type != W32_OBJ_TOKEN || out->c.version != CIUKI_SECURITY_VERSION ||
        out->c.size != sizeof(out->c) || out->c.group_count > CIUKI_SECURITY_GROUP_MAX || (a.value & ~7u))
        return 13; /* ERROR_INVALID_DATA: never present a partial identity */
    out->privileges = a.value;
    return 0;
}
static uint32_t token_open(uint32_t process, uint32_t *handle)
{
    struct ciuki_w32_object_args a = {.action = W32_OBJ_CREATE, .type = W32_OBJ_TOKEN, .source = process,
                                      .access = TOKEN_QUERY};
    uint32_t e = w32r_object(&a, 0);
    if (!e && !a.result)
        e = 6;
    *handle = e ? 0 : a.result;
    return e;
}
static void token_close(uint32_t handle)
{
    struct ciuki_w32_object_args a = {.action = W32_OBJ_CLOSE, .handle = handle};
    (void)w32r_object(&a, 0);
}
/* The calling process's own credential: a transient query handle. */
static uint32_t current(struct identity *out)
{
    uint32_t handle;
    uint32_t e = token_open(W32_CURRENT_PROCESS, &handle);
    if (e)
        return e;
    e = query(handle, out);
    token_close(handle);
    return e;
}

/* Machine SID components, read once per process (immutable per install). */
static uint32_t machine_state; /* 0 unknown, 1 filling, 2 ready */
static uint32_t machine_sid[3];
static uint32_t machine(uint32_t out[3])
{
    if (__atomic_load_n(&machine_state, __ATOMIC_ACQUIRE) == 2) {
        w32_copy(out, machine_sid, sizeof(machine_sid));
        return 0;
    }
    uint32_t e = w32r_machine(out);
    if (e)
        return e;
    if (!out[0] && !out[1] && !out[2])
        return 1355; /* ERROR_NO_SUCH_DOMAIN: no published installation identity */
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&machine_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        w32_copy(machine_sid, out, sizeof(machine_sid));
        __atomic_store_n(&machine_state, 2, __ATOMIC_RELEASE);
    }
    return 0;
}
static uint32_t sid_user(uint8_t *out, uint32_t uid, uint32_t *length)
{
    if (uid == CIUKI_UID_SYSTEM) {
        *length = sid_system(out);
        return 0;
    }
    uint32_t m[3];
    uint32_t e = machine(m);
    if (e)
        return e;
    const uint32_t sub[5] = {21, m[0], m[1], m[2], uid};
    *length = sid_make(out, 5, 5, sub);
    return 0;
}
static uint32_t sid_group(uint8_t *out, uint32_t gid)
{
    if (gid == CIUKI_GID_SYSTEM)
        return sid_system(out);
    if (gid == CIUKI_GID_ADMINISTRATORS || gid == CIUKI_GID_USERS || gid == CIUKI_GID_GUESTS)
        return sid_builtin(out, gid);
    return 0; /* no fabricated SID for an unknown group id */
}

/* ACCOUNTS LOOKUP=1 (user-security-contract.md 4.1): one 312-byte selector
 * body, reply version/count/generation then one 144-byte public record. */
static void put_le32(uint8_t *p, uint32_t v) { w32_put32(p, v); }
static int32_t account_lookup(uint32_t uid, const char *name, uint32_t *uid_out, uint32_t *groups,
                              char out[32])
{
    uint8_t request[312], reply[160];
    uint32_t used = 0, n = 0;
    w32_fill(request, 0, sizeof(request));
    put_le32(request, 1);
    if (name) {
        n = w32_strlen(name);
        if (!n || n > 31)
            return -22;
        put_le32(request + 24, n);
        w32_copy(request + 48, name, n);
    } else
        put_le32(request + 36, uid);
    int32_t e = w32r_accounts(1, request, sizeof(request), reply, sizeof(reply), &used);
    if (e)
        return e;
    if (used != sizeof(reply) || w32_u32(reply) != 1 || w32_u32(reply + 4) != 1 || w32_u32(reply + 28) ||
        (!name && w32_u32(reply + 16) != uid) || !w32_u32(reply + 20) || (w32_u32(reply + 20) & ~7u))
        return -5;
    unsigned length = 0;
    while (length < 32 && reply[32 + length])
        length++;
    if (!length || length == 32)
        return -5;
    for (unsigned i = length; i < 32; i++)
        if (reply[32 + i])
            return -5;
    w32_copy(out, reply + 32, 32);
    if (uid_out)
        *uid_out = w32_u32(reply + 16);
    if (groups)
        *groups = w32_u32(reply + 20);
    return 0;
}

/* ---------------------------------------------------------------- tokens */
int W32API OpenProcessToken(void *process, uint32_t access, void **token)
{
    if (!token)
        return fail(998);
    *token = 0;
    if (access & ~QUERY_RIGHTS)
        return fail(5);
    uint32_t handle;
    uint32_t e = token_open((uint32_t)(uintptr_t)process, &handle);
    if (e)
        return fail(e);
    *token = (void *)(uintptr_t)handle;
    return 1;
}
/* No thread ever impersonates in this subset: there is no thread token. */
int W32API OpenThreadToken(void *thread, uint32_t access, int self, void **token)
{
    (void)thread;
    (void)self;
    if (!token)
        return fail(998);
    *token = 0;
    if (access & ~QUERY_RIGHTS)
        return fail(5);
    return fail(1008); /* ERROR_NO_TOKEN */
}
struct sid_attributes {
    void *sid;
    uint32_t attributes;
};
struct token_groups {
    uint32_t n;
    struct sid_attributes g[];
};
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(struct sid_attributes) == 8, "SID_AND_ATTRIBUTES i386 layout");
_Static_assert(offsetof(struct token_groups, g) == 4, "TOKEN_GROUPS i386 layout");
#endif
int W32API GetTokenInformation(void *token, uint32_t cls, void *buffer, uint32_t length, uint32_t *returned)
{
    if (!returned)
        return fail(998);
    if (cls != TokenUser && cls != TokenGroups && cls != TokenPrivileges && cls != TokenOwner &&
        cls != TokenPrimaryGroup && cls != TokenType && cls != TokenSessionId)
        return fail(87);
    struct identity id;
    uint32_t e = query((uint32_t)(uintptr_t)token, &id);
    if (e)
        return fail(e);
    uint8_t sids[CIUKI_SECURITY_GROUP_MAX][SID_BYTES], one[SID_BYTES];
    uint32_t lengths[CIUKI_SECURITY_GROUP_MAX], count = 0, need = 0, bytes = 0;
    if (cls == TokenUser || cls == TokenOwner) {
        e = sid_user(one, id.c.uid, &bytes);
        if (e)
            return fail(e);
        need = (cls == TokenUser ? sizeof(struct sid_attributes) : sizeof(void *)) + bytes;
    } else if (cls == TokenPrimaryGroup) {
        bytes = sid_group(one, id.c.primary_gid);
        if (!bytes)
            return fail(13);
        need = sizeof(void *) + bytes;
    } else if (cls == TokenGroups) {
        /* The actual membership: built-in groups, or S-1-5-18 for SYSTEM. */
        need = offsetof(struct token_groups, g);
        for (uint32_t i = 0; i < id.c.group_count; i++) {
            uint32_t n = sid_group(sids[count], id.c.groups[i]);
            if (!n)
                continue;
            lengths[count++] = n;
            need += sizeof(struct sid_attributes) + n;
        }
    } else if (cls == TokenPrivileges) {
        for (unsigned bit = 0; bit < 3; bit++)
            count += (id.privileges >> bit) & 1;
        need = 4 + 12 * count;
    } else
        need = 4;
    *returned = need;
    if (!buffer || length < need)
        return fail(122);
    uint8_t *p = buffer;
    switch (cls) {
    case TokenUser: {
        struct sid_attributes *u = buffer;
        u->sid = p + sizeof(*u);
        u->attributes = 0;
        w32_copy(p + sizeof(*u), one, bytes);
        break;
    }
    case TokenOwner:
    case TokenPrimaryGroup:
        *(void **)buffer = p + sizeof(void *);
        w32_copy(p + sizeof(void *), one, bytes);
        break;
    case TokenGroups: {
        struct token_groups *t = buffer;
        uint8_t *at = p + offsetof(struct token_groups, g) + count * sizeof(struct sid_attributes);
        t->n = count;
        for (uint32_t i = 0; i < count; i++) {
            t->g[i].sid = at;
            t->g[i].attributes = 7; /* MANDATORY | ENABLED_BY_DEFAULT | ENABLED */
            w32_copy(at, sids[i], lengths[i]);
            at += lengths[i];
        }
        break;
    }
    case TokenPrivileges: {
        w32_put32(p, count);
        uint8_t *at = p + 4;
        for (unsigned bit = 0; bit < 3; bit++)
            if ((id.privileges >> bit) & 1) {
                w32_put32(at, bit + 1); /* local LUID LowPart */
                w32_put32(at + 4, 0);
                w32_put32(at + 8, 3); /* ENABLED_BY_DEFAULT | ENABLED */
                at += 12;
            }
        break;
    }
    case TokenType:
        w32_put32(p, 1); /* TokenPrimary */
        break;
    default:
        w32_put32(p, id.c.session_id);
        break;
    }
    return 1;
}
/* NULL checks the caller's own credential; an explicit handle is a primary
 * token, which the Microsoft API rejects (ERROR_BAD_TOKEN_TYPE). */
int W32API CheckTokenMembership(void *token, const void *sid, int *member)
{
    if (!member)
        return fail(87);
    uint32_t n = sid_length(sid);
    if (!n)
        return fail(1337);
    struct identity id;
    if (token) {
        uint32_t e = query((uint32_t)(uintptr_t)token, &id);
        return fail(e ? e : 1349);
    }
    uint32_t e = current(&id);
    if (e)
        return fail(e);
    const uint8_t *s = sid;
    uint8_t mine[SID_BYTES];
    int found = 0;
    for (uint32_t i = 0; i < id.c.group_count && !found; i++) {
        uint32_t m = sid_group(mine, id.c.groups[i]);
        found = m == n && same_bytes(mine, s, n);
    }
    /* The user SID itself; the machine SID is fetched only for a candidate
     * with the local-account shape and this uid. */
    if (!found && id.c.uid == CIUKI_UID_SYSTEM)
        found = sid_system(mine) == n && same_bytes(mine, s, n);
    else if (!found && n == 28 && s[1] == 5 && s[7] == 5 && w32_u32(s + 8) == 21 && w32_u32(s + 24) == id.c.uid) {
        uint32_t m;
        e = sid_user(mine, id.c.uid, &m);
        if (e)
            return fail(e);
        found = m == n && same_bytes(mine, s, n);
    }
    *member = found;
    return 1;
}

/* ------------------------------------------------------------ SID helpers */
int W32API AllocateAndInitializeSid(const void *authority, uint8_t count, uint32_t s0, uint32_t s1, uint32_t s2,
                                    uint32_t s3, uint32_t s4, uint32_t s5, uint32_t s6, uint32_t s7, void **sid)
{
    if (!authority || !sid)
        return fail(87);
    if (count > 8)
        return fail(1337);
    const uint32_t sub[8] = {s0, s1, s2, s3, s4, s5, s6, s7};
    uint8_t *p = w32r_alloc(8u + 4u * count);
    if (!p)
        return fail(8);
    p[0] = 1;
    p[1] = count;
    w32_copy(p + 2, authority, 6);
    for (unsigned i = 0; i < count; i++)
        w32_put32(p + 8 + 4 * i, sub[i]);
    *sid = p;
    return 1;
}
/* Only a block this process heap allocated is released; anything else is
 * returned unchanged, FreeSid's documented failure. */
void *W32API FreeSid(void *sid)
{
    if (!sid)
        return 0;
    if (w32r_release(sid))
        return 0;
    w32r_last_error(87);
    return sid;
}
int W32API IsValidSid(const void *sid) { return sid_length(sid) != 0; }
uint32_t W32API GetSidLengthRequired(uint8_t count) { return 8u + 4u * count; }
int W32API InitializeSid(void *sid, const void *authority, uint8_t count)
{
    if (!sid || !authority || count > 15)
        return fail(87);
    uint8_t *p = sid;
    p[0] = 1;
    p[1] = count;
    w32_copy(p + 2, authority, 6);
    return 1;
}
uint32_t *W32API GetSidSubAuthority(void *sid, uint32_t index)
{
    uint8_t *p = sid;
    if (!sid_length(sid) || index >= p[1]) {
        w32r_last_error(1337);
        return 0;
    }
    w32r_last_error(0);
    return (uint32_t *)(void *)(p + 8 + 4 * index);
}
uint8_t *W32API GetSidSubAuthorityCount(void *sid)
{
    if (!sid_length(sid)) {
        w32r_last_error(1337);
        return 0;
    }
    w32r_last_error(0);
    return (uint8_t *)sid + 1;
}
void *W32API GetSidIdentifierAuthority(void *sid)
{
    if (!sid_length(sid)) {
        w32r_last_error(1337);
        return 0;
    }
    w32r_last_error(0);
    return (uint8_t *)sid + 2;
}
int W32API CopySid(uint32_t length, void *destination, const void *source)
{
    uint32_t n = sid_length(source);
    if (!n)
        return fail(1337);
    if (!destination || length < n)
        return fail(122);
    w32_copy(destination, source, n);
    return 1;
}

/* ------------------------------------------------------- local lookups */
/* NULL, empty, or this computer's name (optionally with a leading "\\"). */
static int local_system(const void *system, int wide)
{
    const uint8_t *a = system;
    const WCHAR *w = system;
    unsigned at = 0;
    if (!system)
        return 1;
    if ((wide ? w[0] : a[0]) == '\\' && (wide ? w[1] : a[1]) == '\\')
        at = 2;
    if (!(wide ? w[at] : a[at]))
        return 1;
    char local[32];
    uint32_t n = sizeof(local);
    if (!GetComputerNameA(local, &n))
        return 0;
    for (unsigned i = 0;; i++, at++) {
        uint32_t c = wide ? w[at] : a[at], l = (uint8_t)local[i];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (l >= 'a' && l <= 'z')
            l -= 32;
        if (c != l)
            return 0;
        if (!c)
            return 1;
    }
}
static uint32_t lookup_sid(const uint8_t *s, uint32_t n, char name[32], char domain[32], uint32_t *use)
{
    uint8_t probe[SID_BYTES];
    if (sid_system(probe) == n && same_bytes(probe, s, n)) {
        copy_text(name, 32, "SYSTEM");
        copy_text(domain, 32, "NT AUTHORITY");
        *use = SidTypeWellKnownGroup;
        return 0;
    }
    static const struct {
        uint32_t rid;
        const char *name;
    } builtin[] = {{544, "Administrators"}, {545, "Users"}, {546, "Guests"}};
    for (unsigned i = 0; i < 3; i++)
        if (sid_builtin(probe, builtin[i].rid) == n && same_bytes(probe, s, n)) {
            copy_text(name, 32, builtin[i].name);
            copy_text(domain, 32, "BUILTIN");
            *use = SidTypeAlias;
            return 0;
        }
    if (s[7] != 5 || !same_bytes(s + 2, "\0\0\0\0\0", 5) || (s[1] != 4 && s[1] != 5) || w32_u32(s + 8) != 21)
        return 1332;
    uint32_t m[3], e = machine(m);
    if (e)
        return e;
    if (w32_u32(s + 12) != m[0] || w32_u32(s + 16) != m[1] || w32_u32(s + 20) != m[2])
        return 1332; /* another domain: no network trust here */
    uint32_t size = 32;
    if (!GetComputerNameA(domain, &size))
        return 1332;
    if (s[1] == 4) {
        copy_text(name, 32, domain);
        *use = SidTypeDomain;
        return 0;
    }
    int32_t r = account_lookup(w32_u32(s + 24), 0, 0, 0, name);
    if (r == -3 || r == -2)
        return 1332; /* ERROR_NONE_MAPPED: not a current stored account */
    if (r == -12)
        return 8;
    if (r)
        return 1062;
    *use = SidTypeUser;
    return 0;
}
static int store_text(void *out, uint32_t *capacity, const char *text, int wide, uint32_t need)
{
    (void)capacity;
    for (uint32_t i = 0; i < need; i++) {
        if (wide)
            ((WCHAR *)out)[i] = (WCHAR)w32r_ansi_scalar((uint8_t)text[i]);
        else
            ((char *)out)[i] = text[i];
    }
    return 1;
}
static int lookup_account_sid(const void *system, const void *sid, void *name, uint32_t *name_count, void *domain,
                              uint32_t *domain_count, uint32_t *use, int wide)
{
    if (!name_count || !domain_count)
        return fail(87);
    if (!local_system(system, wide))
        return fail(50);
    uint32_t n = sid_length(sid);
    if (!n)
        return fail(1337);
    char found[32], area[32];
    uint32_t kind = 0, e = lookup_sid(sid, n, found, area, &kind);
    if (e)
        return fail(e);
    uint32_t need_name = w32_strlen(found) + 1, need_domain = w32_strlen(area) + 1;
    if (!name || !domain || *name_count < need_name || *domain_count < need_domain) {
        *name_count = need_name;
        *domain_count = need_domain;
        return fail(122);
    }
    store_text(name, name_count, found, wide, need_name);
    store_text(domain, domain_count, area, wide, need_domain);
    *name_count = need_name - 1;
    *domain_count = need_domain - 1;
    if (use)
        *use = kind;
    return 1;
}
int W32API LookupAccountSidA(const char *system, const void *sid, char *name, uint32_t *name_count, char *domain,
                             uint32_t *domain_count, uint32_t *use)
{
    return lookup_account_sid(system, sid, name, name_count, domain, domain_count, use, 0);
}
int W32API LookupAccountSidW(const WCHAR *system, const void *sid, WCHAR *name, uint32_t *name_count,
                             WCHAR *domain, uint32_t *domain_count, uint32_t *use)
{
    return lookup_account_sid(system, sid, name, name_count, domain, domain_count, use, 1);
}
/* The frozen local privilege names; LUIDs are local opaque values. */
static int lookup_privilege(const void *system, const void *name, void *luid, int wide)
{
    static const char *const names[] = {"SeShutdownPrivilege", "SeSystemtimePrivilege", "SeLoadDriverPrivilege"};
    if (!local_system(system, wide))
        return fail(50);
    if (!name || !luid)
        return fail(87);
    for (unsigned i = 0; i < 3; i++) {
        unsigned j = 0;
        for (;; j++) {
            uint32_t c = wide ? ((const WCHAR *)name)[j] : ((const uint8_t *)name)[j];
            uint32_t want = (uint8_t)names[i][j];
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (want >= 'A' && want <= 'Z')
                want += 32;
            if (c != want)
                break;
            if (!c) {
                w32_put32(luid, i + 1);
                w32_put32((uint8_t *)luid + 4, 0);
                return 1;
            }
        }
    }
    return fail(1313); /* ERROR_NO_SUCH_PRIVILEGE */
}
int W32API LookupPrivilegeValueA(const char *system, const char *name, void *luid)
{
    return lookup_privilege(system, name, luid, 0);
}
int W32API LookupPrivilegeValueW(const WCHAR *system, const WCHAR *name, void *luid)
{
    return lookup_privilege(system, name, luid, 1);
}

/* GetUserName: the authenticated account of the calling process, from the
 * kernel credential and the filtered account record; never USER/HOME or a
 * name the desktop chose. The count includes the terminator. */
static int user_name(void *out, uint32_t *count, int wide)
{
    if (!count)
        return fail(87);
    struct identity id;
    uint32_t e = current(&id);
    if (e)
        return fail(e == 8 ? 8 : 1245);
    char text[32];
    if (id.c.uid == CIUKI_UID_SYSTEM)
        copy_text(text, sizeof(text), "SYSTEM");
    else {
        int32_t r = id.c.uid == CIUKI_UID_INVALID ? -3 : account_lookup(id.c.uid, 0, 0, 0, text);
        if (r)
            return fail(r == -12 ? 8 : 1245); /* ERROR_NOT_LOGGED_ON */
    }
    uint32_t need = w32_strlen(text) + 1;
    if (!out || *count < need) {
        *count = need;
        return fail(122);
    }
    store_text(out, count, text, wide, need);
    *count = need;
    return 1;
}
int W32API GetUserNameA(char *name, uint32_t *count) { return user_name(name, count, 0); }
int W32API GetUserNameW(WCHAR *name, uint32_t *count) { return user_name(name, count, 1); }
