/* sec-k05 DOS identity and host-volume gate. See dos_identity.h.
 * Every credential, session, profile and policy decision comes from the
 * production k01 subject and k02 resolved-object policy; this file keeps no
 * second policy model. A missing service fails closed (EPERM/EACCES); only a
 * kernel-internal VM, which no user can create, may run without a subject.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "dos_identity.h"
#include <ciuki/security.h>
#include <ciuki/files.h>
#ifdef FS_HOST
#include <stdio.h>
#define identity_format snprintf
#else
#include <ciuki/kernel.h>
#define identity_format ksnprintf
#endif
/* Host harnesses link only the parts they exercise; production links all. */
#pragma weak security_subject_get
#pragma weak security_subject_put
#pragma weak security_subject_live
#pragma weak security_subject_matches
#pragma weak security_subject_credential
#pragma weak security_session_profile
#pragma weak proc_supervisor
#pragma weak vfs_resolve_subject_locked
#pragma weak px_policy_check_locked
#pragma weak px_resolve_volume_subject_locked
#pragma weak px_retain
#pragma weak px_release
/* sec-k03 interface (fs/vfs.c), wired by sec-own: bind a retained subject
 * to a table before its first operation; the table holds its own reference
 * until vfs_table_destroy and every later lookup, open, create, find,
 * rename, delete, attribute, time and commit on it is authorized as that
 * subject (user-security-kernel.md K4). Weak for DOS harnesses without it. */
#pragma weak vfs_table_bind_subject

static const char legacy_store[] = "C:\\SYSTEM\\DOS\\SAVES";
static const char legacy_scratch[] = "C:\\SYSTEM\\DOS";
static const char legacy_export[] = "C:\\Documents\\Saves";

static bool administrator(const struct ciuki_credential *c)
{
    if (!c->uid) return true;
    for (unsigned i = 0; i < c->group_count && i < 8; i++)
        if (c->groups[i] == CIUKI_GID_ADMINISTRATORS) return true;
    return false;
}
static bool guest(const struct ciuki_credential *c)
{
    if (c->uid == CIUKI_UID_GUEST) return true;
    for (unsigned i = 0; i < c->group_count && i < 8; i++)
        if (c->groups[i] == CIUKI_GID_GUESTS) return true;
    return false;
}
/* The session's committed root is used verbatim (A2-8): never derived
 * from the uid, so a migrated uid keeps its retained profile spelling. */
static int profile_root(struct dos_identity *id, const struct ciuki_security_profile *p)
{
    id->profile[0] = 0;
    if (!p->path_bytes) return 0; /* interim bootstrap session: legacy machine store */
    if (p->path_bytes > sizeof(id->profile) || p->path[p->path_bytes - 1]) return -EIO;
    static const char prefix[] = "/Windows/Profiles/";
    if (strncmp(p->path, prefix, sizeof(prefix) - 1)) return -EIO;
    for (unsigned i = 0; i + 1 < p->path_bytes; i++) {
        char c = p->path[i];
        if (c < 33 || c > 126 || c == '\\' || c == ':' || (c == '/' && p->path[i + 1] == '/')) return -EIO;
    }
    memcpy(id->profile, p->path, p->path_bytes);
    return 0;
}
int dos_identity_capture(struct dos_identity *id, struct process *p)
{
    memset(id, 0, sizeof(*id));
    if (!p || !p->subject || !security_subject_get || !security_subject_put || !security_subject_live ||
        !security_subject_credential || !security_session_profile) return -EPERM;
    struct security_subject *s = p->subject;
    const struct ciuki_credential *c = security_subject_credential(s);
    /* Own-session class: an authenticated interactive session. SYSTEM
     * services (session 0) have no DOS identity of their own. */
    if (!c || !security_subject_live(s) || !c->session_id || !(c->flags & CIUKI_CRED_INTERACTIVE) ||
        c->uid == CIUKI_UID_SYSTEM || c->uid == CIUKI_UID_INVALID || !c->group_count || c->group_count > 8)
        return -EPERM;
    struct ciuki_security_profile profile;
    int e = security_session_profile(c->session_id, &profile);
    if (e) return -EPERM;
    if (profile.uid != c->uid) return -EACCES;
    struct dos_identity next = { .credential = *c, .admin = administrator(c), .guest = guest(c) };
    if ((e = profile_root(&next, &profile))) return e;
    /* The interim bootstrap desktop (no committed profile) is an
     * administrator; any other session without a profile is refused. */
    if (!next.profile[0] && !next.admin) return -EACCES;
    if ((e = security_subject_get(s))) return e;
    next.subject = s;
    *id = next;
    return 0;
}
void dos_identity_internal(struct dos_identity *id)
{
    memset(id, 0, sizeof(*id));
    id->system = id->admin = true;
    id->credential = (struct ciuki_credential){ .version = 1, .size = 64, .uid = CIUKI_UID_SYSTEM,
        .primary_gid = CIUKI_GID_SYSTEM, .group_count = 1 };
    struct process *pid1 = proc_supervisor ? proc_supervisor() : 0;
    const struct ciuki_credential *c = pid1 && pid1->subject && security_subject_credential ?
        security_subject_credential(pid1->subject) : 0;
    if (c && c->uid == CIUKI_UID_SYSTEM && !c->session_id && security_subject_get &&
        !security_subject_get(pid1->subject))
        id->subject = pid1->subject;
}
int dos_identity_copy(struct dos_identity *to, const struct dos_identity *from)
{
    *to = *from;
    if (!from->subject) return 0;
    int e = security_subject_get ? security_subject_get(from->subject) : -EACCES;
    if (e) memset(to, 0, sizeof(*to));
    return e;
}
void dos_identity_release(struct dos_identity *id)
{
    if (id->subject && security_subject_put) security_subject_put(id->subject);
    memset(id, 0, sizeof(*id));
}
bool dos_identity_live(const struct dos_identity *id)
{
    if (id->system) return true;
    return id->subject && security_subject_live && security_subject_live(id->subject);
}
bool dos_identity_owner(const struct dos_identity *id, const struct process *p)
{
    return !id->system && p && dos_identity_live(id) && security_subject_matches &&
        security_subject_matches(id->subject, p);
}
static const struct ciuki_credential *process_credential(const struct process *p)
{
    if (!p || !p->subject || !security_subject_live || !security_subject_credential ||
        !security_subject_live(p->subject)) return 0;
    const struct ciuki_credential *c = security_subject_credential(p->subject);
    return c && c->group_count <= 8 ? c : 0;
}
bool dos_process_interactive(const struct process *p)
{
    const struct ciuki_credential *c = process_credential(p);
    return c && c->session_id && (c->flags & CIUKI_CRED_INTERACTIVE) && c->uid != CIUKI_UID_SYSTEM &&
        c->uid != CIUKI_UID_INVALID;
}
bool dos_process_admin(const struct process *p)
{
    const struct ciuki_credential *c = process_credential(p);
    return c && administrator(c);
}
static int append(char *out, size_t size, size_t *used, const char *text)
{
    size_t n = strlen(text);
    if (n >= size - *used) return -FS_ENAMETOOLONG;
    memcpy(out + *used, text, n + 1); *used += n;
    return 0;
}
int dos_identity_root(const struct dos_identity *id, enum dos_identity_root which, char *out, size_t size)
{
    if (!size) return -FS_ENAMETOOLONG;
    out[0] = 0;
    size_t used = 0;
    if (!id->profile[0]) {
        const char *legacy = which == DOS_ROOT_STORE ? legacy_store :
                             which == DOS_ROOT_SCRATCH ? legacy_scratch : legacy_export;
        return append(out, size, &used, legacy);
    }
    int e = append(out, size, &used, "C:");
    if (!e) e = append(out, size, &used, id->profile);
    for (size_t i = 2; !e && i < used; i++) if (out[i] == '/') out[i] = '\\';
    /* Short components under PROFILE(uid): Settings is the protected
     * per-profile settings anchor; DOS holds this user's private C: data. */
    if (!e) e = append(out, size, &used, which == DOS_ROOT_EXPORT ? "\\Documents\\Saves" : "\\Settings\\DOS");
    if (e) out[0] = 0;
    return e;
}
int dos_identity_export_name(const struct dos_identity *id, const char *folder, char out[32])
{
    char root[DOS_IDENTITY_PATH];
    int e = dos_identity_root(id, DOS_ROOT_EXPORT, root, sizeof(root));
    if (e) return e;
    size_t n = strlen(root);
    if (strncmp(folder, root, n) || (folder[n] && folder[n] != '\\')) return -FS_EINVAL;
    const char *label = id->profile[0] ? "Documents\\Saves" : root;
    int count = identity_format(out, 32, "%s%s", label, folder + n);
    return count < 0 || count >= 32 ? -FS_ENAMETOOLONG : 0;
}
int dos_identity_prepare(const struct dos_identity *id, struct vfs_table *t, enum dos_identity_root which)
{
    char root[DOS_IDENTITY_PATH];
    int e = dos_identity_root(id, which, root, sizeof(root));
    if (e) return e;
    /* Create only below the profile root (or the legacy parent), which
     * the account/owner service provisions; never the profile itself. */
    size_t base = id->profile[0] ? 2 + strlen(id->profile) : strlen(root);
    if (!id->profile[0]) {
        while (base > 2 && root[base - 1] != '\\') base--;
        if (base > 2) base--;
    }
    for (size_t i = base + 1; ; i++) {
        if (root[i] != '\\' && root[i]) continue;
        char saved = root[i]; root[i] = 0;
        e = vfs_mkdir(t, root, 0);
        root[i] = saved;
        if (e && e != -FS_EEXIST && e != -FS_EROFS) return e;
        if (!saved) return 0;
    }
}
int dos_identity_bind(const struct dos_identity *id, struct vfs_table *t)
{
    /* Standalone DOS harnesses link neither table subjects nor credentials;
     * their tables stay kernel work, as before. The kernel links both. */
    if (!vfs_table_bind_subject || !security_subject_get) return 0;
    if (!id->subject) return id->system ? 0 : -FS_EACCES;
    /* A table already bound to this VM's identity keeps it (idempotent). */
    if (t->subject == id->subject) return 0;
    int e = vfs_table_bind_subject(t, id->subject);
    return e ? -FS_EACCES : 0;
}

/* ---- host-volume gate ---------------------------------------------- */
static const struct dos_identity *gate_identity(const struct dos_host_gate *g)
{
    const struct dos_identity *id = g ? g->identity : 0;
    return id && id->subject && !id->system ? id : 0;
}
/* Authorized-but-absent lookups keep their DOS meaning (2/3); everything
 * else is a denial: EACCES, a revoked subject (ESRCH), a missing policy
 * binding or owner metadata (EIO from the label), EPERM. */
static int gate_error(int e)
{
    switch (e) {
    case 0: case -FS_ENOENT: case -FS_ENOTDIR: case -FS_EINVAL: case -FS_ENAMETOOLONG: case -FS_EILSEQ:
        return e;
    default: return -FS_EACCES;
    }
}
#define GATE_ATTEMPTS 4u /* EAGAIN: a mount alias attached during admission */
static int gate_check(const struct dos_host_gate *g, struct vfs_table *t, const char *native, unsigned access)
{
    const struct dos_identity *id = gate_identity(g);
    if (!id || !t || !native || !access || !vfs_resolve_subject_locked || !px_policy_check_locked) return -FS_EACCES;
    int e = -FS_EAGAIN;
    for (unsigned attempt = 0; e == -FS_EAGAIN && attempt < GATE_ATTEMPTS; attempt++) {
        struct px_path r;
        vfs_lock(t->vfs, 0);
        /* Traverses every real component, then requires READ on the leaf. */
        e = vfs_resolve_subject_locked(t, id->subject, native, false, false, &r);
        if (!e && (access & ~(unsigned)POLICY_READ)) e = px_policy_check_locked(r.node, id->subject, access);
        vfs_unlock(t->vfs);
    }
    return gate_error(e);
}
static int gate_retain(const struct dos_host_gate *g, struct vfs_table *t, const char *native, unsigned access,
                       int handle, void **object)
{
    const struct dos_identity *id = gate_identity(g);
    *object = 0;
    if (!id || !t || !native || !access || !vfs_resolve_subject_locked || !px_policy_check_locked ||
        !px_retain || !px_release) return -FS_EACCES;
    struct fat_entry opened; uint64_t position;
    int e = handle >= 0 ? vfs_handle_stat(t, handle, &opened, &position) : 0;
    if (e) return gate_error(e);
    e = -FS_EAGAIN;
    for (unsigned attempt = 0; e == -FS_EAGAIN && attempt < GATE_ATTEMPTS; attempt++) {
        struct px_path r;
        vfs_lock(t->vfs, 0);
        e = vfs_resolve_subject_locked(t, id->subject, native, false, false, &r);
        if (!e && (access & ~(unsigned)POLICY_READ)) e = px_policy_check_locked(r.node, id->subject, access);
        /* After an open the handle must be the object the path names for
         * this subject now: same mounted volume and directory slot. */
        if (!e && handle >= 0 && (!r.node->volume || r.node->entry.parent != opened.parent ||
            r.node->entry.index != opened.index || r.node->drive >= 26 ||
            t->vfs->volumes[r.node->drive] != r.node->volume)) e = -FS_EACCES;
        if (!e) { px_retain(r.node); *object = r.node; }
        vfs_unlock(t->vfs);
    }
    return gate_error(e);
}
static int gate_child(const struct dos_host_gate *g, struct vfs_table *t, void *directory, const char *name, unsigned access)
{
    const struct dos_identity *id = gate_identity(g);
    struct px_node *d = directory;
    if (!id || !d || !name || !access || !px_resolve_volume_subject_locked || !px_policy_check_locked) return -FS_EACCES;
    int e = -FS_EAGAIN;
    for (unsigned attempt = 0; e == -FS_EAGAIN && attempt < GATE_ATTEMPTS; attempt++) {
        struct px_path r;
        vfs_lock(t->vfs, 0);
        /* Relative to the held directory: TRAVERSE it, then READ the entry. */
        e = px_resolve_volume_subject_locked(d->space, d->drive, d, name, false, false, id->subject, &r);
        if (!e && (access & ~(unsigned)POLICY_READ)) e = px_policy_check_locked(r.node, id->subject, access);
        vfs_unlock(t->vfs);
    }
    return gate_error(e);
}
static int gate_recheck(const struct dos_host_gate *g, struct vfs_table *t, void *object, unsigned access)
{
    const struct dos_identity *id = gate_identity(g);
    if (!id || !object || !px_policy_check_locked) return -FS_EACCES;
    vfs_lock(t->vfs, 0);
    int e = px_policy_check_locked(object, id->subject, access);
    vfs_unlock(t->vfs);
    return gate_error(e);
}
static void gate_release(const struct dos_host_gate *g, struct vfs_table *t, void *object)
{
    (void)g;
    if (!object || !px_release) return;
    vfs_lock(t->vfs, 0);
    px_release(object);
    vfs_unlock(t->vfs);
}
void dos_identity_gate(const struct dos_identity *id, struct dos_host_gate *g)
{
    *g = (struct dos_host_gate){ gate_check, gate_retain, gate_child, gate_recheck, gate_release, id };
}
