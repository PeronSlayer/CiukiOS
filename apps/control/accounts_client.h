/* SPDX-License-Identifier: MIT
 * sec-user-04: the Control Panel's client of the SYSTEM account service
 * (ACCOUNTS, call-101 service 2) for User Accounts. Not an SDK ABI.
 *
 * Contract: user-security-contract.md 4.1 (management 312-byte body with
 * client_operation_id@304, RESERVE 48/16, STATUS 32/80, filtered public
 * records 144, A2-4/A2-6 durable operation ids) and 7.1 (User Accounts).
 * The client holds no authority: the service authorizes every request with
 * the kernel-retained credential of this process's link, re-checking the
 * caller's role, last-administrator and active-account rules when the change
 * is applied, not when the page was opened. The caller's own role comes from
 * the kernel (QUERY_SELF), never from a request field or the environment.
 *
 * One cpa_execute call: QUERY_SELF, LIST (management view for an
 * administrator, enabled accounts otherwise), and for a change RESERVE ->
 * management -> (lost reply) STATUS -> LIST. Secrets are wiped from the
 * request and every local copy on every path; they are never logged.
 */
#ifndef CIUKI_CONTROL_ACCOUNTS_CLIENT_H
#define CIUKI_CONTROL_ACCOUNTS_CLIENT_H
#include <stddef.h>
#include <stdint.h>

#define CPA_MAX 18u
#define CPA_NAME 32u
#define CPA_PICTURE_MAX 96u
#define CPA_SECRET 64u
#define CPA_ENABLED 1u                 /* public record flags */
#define CPA_ADMIN 2u
#define CPA_GROUP_ADMINS 544u
#define CPA_GROUP_USERS 545u
#define CPA_GROUP_GUESTS 546u
#define CPA_UID_RECOVERY 500u          /* CiukiAdministrator */
#define CPA_UID_GUEST 501u

struct cpa_record {
    uint32_t uid, flags, groups;       /* groups: 3 administrator, 2 standard, 4 guest (service roles) */
    char name[CPA_NAME], picture[CPA_PICTURE_MAX];
};
struct cpa_view {
    uint64_t generation;               /* database generation of this snapshot */
    unsigned count;
    struct cpa_record records[CPA_MAX];
    uint32_t self_uid;                 /* this process's kernel credential */
    int self_admin, self_guest, valid;
};
enum cpa_action { CPA_READ, CPA_CREATE, CPA_DELETE, CPA_RENAME, CPA_TYPE, CPA_PASSWORD, CPA_PICTURE, CPA_ENABLE };
struct cpa_request {
    unsigned action;
    uint32_t uid;                      /* target (0 for CREATE and READ) */
    int admin;                         /* CREATE/TYPE: administrator instead of standard user */
    int enable;                        /* ENABLE: 1 turn on, 0 turn off; CREATE: account enabled */
    char name[CPA_NAME], picture[CPA_PICTURE_MAX];
    char old_password[CPA_SECRET], password[CPA_SECRET], confirm[CPA_SECRET];
};
struct cpa_transport {
    void *ctx;
    /* One ACCOUNTS exchange at frame generation: 0 with *status (0 or
     * -errno from the service), or a negative transport error (no reply). */
    int (*call)(void *, uint32_t operation, uint64_t generation, const void *body, size_t bytes,
                void *reply, size_t capacity, size_t *used, int *status);
    /* The caller's kernel credential: uid and supplementary groups. */
    int (*self)(void *, uint32_t *uid, uint32_t groups[8], unsigned *count);
};
/* 0, or -errno: -EPERM role/last-administrator/built-in rule, -EBUSY account
 * in use, -EACCES old password, -EINVAL name/password/confirmation,
 * -EAGAIN the list changed (view refreshed), -ENOSPC no account slot,
 * -EIO unknown outcome after the reply was lost. view is refreshed whenever
 * a LIST succeeded. The request's secrets are always wiped. */
int cpa_execute(const struct cpa_transport *, struct cpa_request *, struct cpa_view *);
/* Header-only helpers (app.c's host builds link no accounts_client.c). */
static inline void cpa_wipe(void *p, size_t n) { volatile uint8_t *b=p; while (n--) *b++=0; }
static inline const struct cpa_record *cpa_find(const struct cpa_view *v, uint32_t uid)
{
    for (unsigned i=0;v && i<v->count;i++) if (v->records[i].uid==uid) return &v->records[i];
    return (const struct cpa_record *)0;
}
/* Account management view for this caller: 1 when the action is offered
 * (the service still decides at apply time). Built-in accounts keep their
 * name and type; nobody deletes, demotes or turns off his own account. */
static inline int cpa_allowed(const struct cpa_view *v, unsigned action, const struct cpa_record *t)
{
    if (!v || !v->valid) return 0;
    int self=t && t->uid==v->self_uid, admin=v->self_admin;
    int builtin=t && (t->uid==CPA_UID_RECOVERY || t->uid==CPA_UID_GUEST);
    switch (action) {
    case CPA_READ: return 1;
    case CPA_CREATE: return admin;
    case CPA_DELETE: return admin && t && !builtin && !self;
    case CPA_RENAME: return admin && t && !builtin;
    case CPA_TYPE: return admin && t && !builtin && !self;
    case CPA_PASSWORD: case CPA_PICTURE: return t && (self || admin);
    case CPA_ENABLE: return admin && t && !self;
    default: return 0;
    }
}
/* The 312-byte management body and the operation digest the service
 * computes (protocol.c acct_management_digest), exposed for host tests. */
int cpa_management_encode(uint32_t wire, uint32_t flags, uint32_t uid, uint64_t generation, const char *name,
                          const char *old_password, const char *password, const char *picture,
                          uint64_t operation_id, uint8_t out[312], uint8_t digest[32]);
#endif
