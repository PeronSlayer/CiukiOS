/* sec-user-06: the sectest model (sectest_model.h). No OS calls.
 * SPDX-License-Identifier: MIT */
#include "sectest_model.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GID_SYSTEM 0u
#define GID_ADMINISTRATORS 544u
#define GID_USERS 545u
#define GID_GUESTS 546u
#define UID_GUEST 501u

static const char *const role_names[ST_ROLES] = { "system", "admin", "standard", "guest", "unknown" };
static const char *const check_names[ST_CHECKS] = {
    "environment", "own-profile", "other-profile", "programs", "root-folder", "foreign-folder",
    "installer-store", "installer-shortcuts", "machine-registry", "machine-setting", "raw-disk",
    "account-store", "backups"
};
/* Rows: enum st_check; columns: admin, standard, guest. The managed-volume
 * policy (src/kernel/fs/security_policy.c policy_check): a profile is its
 * owner's and the administrators' (K-Q1), /Programs is PROGRAM (administrator
 * writes), the volume root admits a Standard user's creator-owned folder and
 * nothing from Guest (M-Q7), /System/Security and /System/SysBckup are
 * SECRET (SYSTEM only). The machine registry is written by administrators
 * through the broker (apps/registry/broker.c native), SETTING is for
 * administrators (proc/syscalls_sysvc.c system_setting; an administrator's
 * rewrite is not attempted), and raw access to the managed system disk is
 * refused to everyone (K-Q4, fs/disklease.c). */
static const unsigned char table[ST_CHECKS][3] = {
    [ST_ENVIRONMENT] = { ST_ALLOW, ST_ALLOW, ST_ALLOW },
    [ST_OWN_PROFILE] = { ST_ALLOW, ST_ALLOW, ST_ALLOW },
    [ST_OTHER_PROFILE] = { ST_ALLOW, ST_DENY, ST_DENY },
    [ST_PROGRAMS] = { ST_ALLOW, ST_DENY, ST_DENY },
    [ST_ROOT_FOLDER] = { ST_ALLOW, ST_ALLOW, ST_DENY },
    [ST_FOREIGN_FOLDER] = { ST_ALLOW, ST_DENY, ST_DENY },
    [ST_INSTALLER_STORE] = { ST_ALLOW, ST_DENY, ST_DENY },
    [ST_INSTALLER_SHORTCUTS] = { ST_ALLOW, ST_DENY, ST_DENY },
    [ST_MACHINE_REGISTRY] = { ST_ALLOW, ST_DENY, ST_DENY },
    [ST_MACHINE_SETTING] = { ST_NOT_ATTEMPTED, ST_DENY, ST_DENY },
    [ST_RAW_DISK] = { ST_DENY, ST_DENY, ST_DENY },
    [ST_ACCOUNT_STORE] = { ST_DENY, ST_DENY, ST_DENY },
    [ST_BACKUPS] = { ST_DENY, ST_DENY, ST_DENY },
};

const char *st_role_name(int role) { return role >= 0 && role < ST_ROLES ? role_names[role] : "unknown"; }
int st_role_parse(const char *name)
{
    for (int i = 0; name && i < ST_ROLES; i++) if (!strcmp(name, role_names[i])) return i;
    return -1;
}
const char *st_check_name(enum st_check c) { return (unsigned)c < ST_CHECKS ? check_names[c] : "unknown"; }
int st_check_parse(const char *name)
{
    for (int i = 0; name && i < ST_CHECKS; i++) if (!strcmp(name, check_names[i])) return i;
    return -1;
}
int st_role_of(const struct st_identity *id)
{
    if (!id || id->count > ST_GROUPS_MAX) return ST_UNKNOWN;
    int admin = 0, users = 0, guests = id->uid == UID_GUEST;
    for (uint32_t i = 0; i < id->count; i++) {
        if (id->groups[i] == GID_SYSTEM) return ST_SYSTEM;
        admin |= id->groups[i] == GID_ADMINISTRATORS;
        users |= id->groups[i] == GID_USERS;
        guests |= id->groups[i] == GID_GUESTS;
    }
    if (!id->uid || id->gid == GID_SYSTEM) return ST_SYSTEM;
    if (admin) return ST_ADMIN;
    if (guests) return ST_GUEST;
    return users ? ST_STANDARD : ST_UNKNOWN;
}
enum st_expect st_expect(int role, enum st_check c)
{
    if ((unsigned)c >= ST_CHECKS || role < ST_ADMIN || role > ST_GUEST) return ST_NOT_ATTEMPTED;
    return (enum st_expect)table[c][role - ST_ADMIN];
}
int st_denied(int result) { return result == -EACCES || result == -EPERM || result == -EROFS; }
static int absent(int result) { return result == -ENOENT || result == -ENODEV || result == -ENXIO; }

static void emit(const struct st_ops *ops, const char *fmt, ...)
{
    char line[ST_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    ops->line(ops->ctx, line, (size_t)n);
}
/* Folder names: 1..31 of [A-Za-z0-9_-], no path. */
static int folder_valid(const char *s)
{
    size_t n = 0;
    for (; s && s[n]; n++) {
        char c = s[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return n && n <= ST_FOLDER_MAX;
}
int st_options_parse(int argc, char **argv, struct st_options *o, int *saver)
{
    memset(o, 0, sizeof(*o)); o->expect = -1;
    if (saver) *saver = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--expect=", 9)) {
            o->expect = st_role_parse(a + 9);
            if (o->expect < ST_ADMIN || o->expect > ST_GUEST) return -1;
        } else if (!strncmp(a, "--folder=", 9)) {
            if (!folder_valid(a + 9)) return -1;
            o->folder = a + 9;
        } else if (!strncmp(a, "--foreign=", 10)) {
            if (!folder_valid(a + 10)) return -1;
            o->foreign = a + 10;
        } else if (!strcmp(a, "--keep")) o->keep = 1;
        else if (!strncmp(a, "--check=", 8)) {
            int c = st_check_parse(a + 8);
            if (c < 0) return -1;
            o->mask |= 1u << c;
        } else if (!strcmp(a, "saver") && saver) *saver = 1;
        else if (strcmp(a, "all")) return -1;
    }
    return 0;
}
static void groups_text(const struct st_identity *id, char *out, size_t size)
{
    size_t used = 0; out[0] = 0;
    for (uint32_t i = 0; i < id->count && i < ST_GROUPS_MAX; i++) {
        int n = snprintf(out + used, size - used, "%s%u", i ? "," : "", (unsigned)id->groups[i]);
        if (n < 0 || (size_t)n >= size - used) break;
        used += (size_t)n;
    }
    if (!used) snprintf(out, size, "-");
}
static int run_check(const struct st_ops *ops, const struct st_options *o, int role, enum st_check c)
{
    switch (c) {
    case ST_ENVIRONMENT: { char leaf[16]; return ops->environment(ops->ctx, leaf, sizeof(leaf)); }
    case ST_OWN_PROFILE: return ops->own_profile(ops->ctx);
    case ST_OTHER_PROFILE: return ops->other_profile(ops->ctx);
    case ST_PROGRAMS: return ops->programs(ops->ctx);
    case ST_ROOT_FOLDER:
        return ops->root_folder(ops->ctx, o->folder ? o->folder : role == ST_GUEST ? "SECTEST" : "GAME", o->keep);
    case ST_FOREIGN_FOLDER: return ops->foreign_folder(ops->ctx, o->foreign);
    case ST_INSTALLER_STORE: return ops->installer_store(ops->ctx);
    case ST_INSTALLER_SHORTCUTS: return ops->installer_shortcuts(ops->ctx);
    case ST_MACHINE_REGISTRY: return ops->machine_registry(ops->ctx);
    case ST_MACHINE_SETTING: return ops->machine_setting(ops->ctx);
    case ST_RAW_DISK: return ops->raw_disk(ops->ctx);
    case ST_ACCOUNT_STORE: return ops->account_store(ops->ctx);
    case ST_BACKUPS: return ops->backups(ops->ctx);
    default: return -EINVAL;
    }
}
int st_run(const struct st_ops *ops, const struct st_options *o, struct st_summary *s)
{
    memset(s, 0, sizeof(*s));
    struct st_identity id; memset(&id, 0, sizeof(id));
    int e = ops->identity(ops->ctx, &id);
    char groups[64];
    groups_text(&id, groups, sizeof(groups));
    s->role = e ? ST_UNKNOWN : st_role_of(&id);
    s->system = !e && s->role == ST_SYSTEM;
    int identity_ok = !e && !s->system && s->role != ST_UNKNOWN && (o->expect < 0 || o->expect == s->role);
    char leaf[16] = "-";
    if (!e && ops->environment(ops->ctx, leaf, sizeof(leaf)) == -ENOENT) snprintf(leaf, sizeof(leaf), "-");
    emit(ops, "sectest begin version=1 uid=%u gid=%u session=%u groups=%s role=%s expect=%s profile=%s result=%d verdict=%s",
         (unsigned)id.uid, (unsigned)id.gid, (unsigned)id.session, groups, st_role_name(s->role),
         o->expect < 0 ? "-" : st_role_name(o->expect), leaf[0] ? leaf : "-", e, identity_ok ? "pass" : "fail");
    s->checks++;
    if (identity_ok) s->passed++; else s->failed++;
    /* A SYSTEM or unrecognised identity attempts nothing (it would prove nothing). */
    for (int c = 0; identity_ok && c < ST_CHECKS; c++) {
        if (o->mask && !(o->mask & (1u << c))) continue;
        if (c == ST_FOREIGN_FOLDER && !o->foreign) continue;   /* only with --foreign=NAME */
        enum st_expect want = st_expect(s->role, (enum st_check)c);
        const char *name = st_check_name((enum st_check)c);
        s->checks++;
        if (want == ST_NOT_ATTEMPTED) {
            s->skipped++;
            emit(ops, "sectest check=%s expect=none result=0 verdict=skip reason=not_attempted_for_%s", name, st_role_name(s->role));
            continue;
        }
        int r = run_check(ops, o, s->role, (enum st_check)c);
        const char *verdict, *reason = NULL;
        if (absent(r) && c != ST_ROOT_FOLDER && c != ST_PROGRAMS && c != ST_OWN_PROFILE && c != ST_ENVIRONMENT) {
            verdict = "skip"; reason = "absent"; s->skipped++;
        } else if (want == ST_ALLOW ? r == 0 : st_denied(r)) {
            verdict = "pass"; s->passed++;
        } else {
            verdict = "fail"; s->failed++;
            reason = want == ST_ALLOW ? (st_denied(r) ? "refused" : "error") : (r == 0 ? "allowed" : "error");
        }
        if (reason) emit(ops, "sectest check=%s expect=%s result=%d verdict=%s reason=%s", name, want == ST_ALLOW ? "allow" : "deny", r, verdict, reason);
        else emit(ops, "sectest check=%s expect=%s result=%d verdict=%s", name, want == ST_ALLOW ? "allow" : "deny", r, verdict);
    }
    emit(ops, "sectest end role=%s checks=%u passed=%u failed=%u skipped=%u", st_role_name(s->role),
         s->checks, s->passed, s->failed, s->skipped);
    return s->failed ? 1 : 0;
}
int st_saver(const struct st_ops *ops)
{
    int r = ops->saver(ops->ctx);
    emit(ops, "sectest saver result=%d", r);
    return r ? 1 : 0;
}
