/* SPDX-License-Identifier: MIT
 * sec-user-05 (user-security-contract.md 8.4 and 4.1 "Authorized profile
 * information"): getpwnam/getpwuid and their _r variants through the SYSTEM
 * account service, never a password file. LOOKUP supplies the public name,
 * uid and group membership; the authorized PROFILE query supplies the
 * committed profile home (<root>/Documents) and the fixed shell. The service
 * answers only the caller's own mapping (any mapping for an administrator),
 * so another user's private paths are EACCES in this subset. pw_passwd is
 * "*": no verifier reaches a process. uid 0 is SYSTEM, not an account. The
 * primary gid follows the kernel credential rule: Guests (546) for a Guest
 * account, Users (545) otherwise; administrators are not POSIX root. */
#include <ciuki/abi.h>
#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <string.h>
int __ciuki_account_rpc(uint32_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
enum { ACCOUNTS_LOOKUP = 1, ACCOUNTS_PROFILE = 8 };
struct account_entry {
    uint32_t uid, gid;
    char name[32], home[96], shell[80];
};
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* A NUL-terminated, zero-padded fixed field. */
static int field(const uint8_t *p, size_t n)
{
    const uint8_t *end = memchr(p, 0, n);
    if (!end || end == p) return 0;
    for (const uint8_t *q = end; q < p + n; q++) if (*q) return 0;
    return 1;
}
static int same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static void select_body(uint8_t body[312], uint32_t uid, const char *name)
{
    memset(body, 0, 312);
    wr32(body, 1);
    if (name) {
        size_t n = strlen(name);
        wr32(body + 24, (uint32_t)n);
        memcpy(body + 48, name, n);
    } else wr32(body + 36, uid);
}
/* 0 found, ENOENT unknown, or the service/transport error. */
static int fetch(uint32_t uid, const char *name, struct account_entry *e)
{
    memset(e, 0, sizeof(*e));
    if (name) {
        size_t n = strlen(name);
        if (!n || n > 31) return ENOENT;
        if (same_name(name, "SYSTEM")) { uid = CIUKI_UID_SYSTEM; name = NULL; }
    }
    if (!name && uid == CIUKI_UID_SYSTEM) {
        e->uid = CIUKI_UID_SYSTEM; e->gid = CIUKI_GID_SYSTEM;
        strcpy(e->name, "SYSTEM"); strcpy(e->home, "/"); strcpy(e->shell, "/Programs/terminal");
        return 0;
    }
    if (!name && uid == CIUKI_UID_INVALID) return ENOENT;
    uint8_t body[312], reply[272];
    uint32_t used = 0;
    select_body(body, uid, name);
    int r = __ciuki_account_rpc(ACCOUNTS_LOOKUP, body, sizeof(body), reply, 160, &used);
    if (r) return r == ESRCH || r == ENOENT ? ENOENT : r;
    uint32_t groups = rd32(reply + 20);
    if (used != 160 || rd32(reply) != 1 || rd32(reply + 4) != 1 || rd32(reply + 28) || !groups || (groups & ~7u) ||
        (!name && rd32(reply + 16) != uid) || !field(reply + 32, 32) || (name && !same_name((const char *)reply + 32, name)))
        return EIO;
    e->uid = rd32(reply + 16);
    e->gid = groups & 4u ? CIUKI_GID_GUESTS : CIUKI_GID_USERS;
    memcpy(e->name, reply + 32, 32);
    select_body(body, e->uid, NULL);
    r = __ciuki_account_rpc(ACCOUNTS_PROFILE, body, sizeof(body), reply, sizeof(reply), &used);
    if (r) return r == ESRCH ? ENOENT : r;
    if (used != 272 || rd32(reply) != 1 || rd32(reply + 4) != 272 || rd32(reply + 8) != e->uid || rd32(reply + 12) ||
        !field(reply + 32, 64) || !field(reply + 96, 96) || !field(reply + 192, 80) || reply[96] != '/' ||
        reply[192] != '/')
        return EIO;
    memcpy(e->home, reply + 96, 96);
    memcpy(e->shell, reply + 192, 80);
    return 0;
}
/* Copies the entry into caller storage; ERANGE without partial output. */
static int fill(const struct account_entry *e, struct passwd *pwd, char *buffer, size_t size)
{
    size_t n = strlen(e->name) + 1, h = strlen(e->home) + 1, s = strlen(e->shell) + 1;
    if (!buffer || size < n + h + s + 2) return ERANGE;
    char *at = buffer;
    memset(pwd, 0, sizeof(*pwd));
    pwd->pw_name = memcpy(at, e->name, n); at += n;
    pwd->pw_passwd = at; *at++ = '*'; *at++ = 0;
    pwd->pw_comment = pwd->pw_gecos = pwd->pw_name;
    pwd->pw_dir = memcpy(at, e->home, h); at += h;
    pwd->pw_shell = memcpy(at, e->shell, s);
    pwd->pw_uid = (uid_t)e->uid;
    pwd->pw_gid = (gid_t)e->gid;
    return 0;
}
static int lookup_r(uint32_t uid, const char *name, struct passwd *pwd, char *buffer, size_t size, struct passwd **result)
{
    if (!result) return EINVAL;
    *result = NULL;
    if (!pwd) return EINVAL;
    struct account_entry e;
    int r = fetch(uid, name, &e);
    if (r == ENOENT) return 0;
    if (!r) r = fill(&e, pwd, buffer, size);
    if (!r) *result = pwd;
    return r;
}
int getpwnam_r(const char *name, struct passwd *pwd, char *buffer, size_t size, struct passwd **result)
{
    if (!name) { if (result) *result = NULL; return EINVAL; }
    return lookup_r(0, name, pwd, buffer, size, result);
}
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buffer, size_t size, struct passwd **result)
{
    return lookup_r((uint32_t)uid, NULL, pwd, buffer, size, result);
}
static struct passwd shared_entry;
static char shared_text[32 + 2 + 96 + 80];
static struct passwd *lookup(uint32_t uid, const char *name)
{
    struct passwd *found = NULL;
    int r = name ? getpwnam_r(name, &shared_entry, shared_text, sizeof(shared_text), &found)
                 : getpwuid_r((uid_t)uid, &shared_entry, shared_text, sizeof(shared_text), &found);
    if (r) errno = r;
    return found;
}
struct passwd *getpwnam(const char *name) { return name ? lookup(0, name) : (errno = EINVAL, (struct passwd *)NULL); }
struct passwd *getpwuid(uid_t uid) { return lookup((uint32_t)uid, NULL); }
