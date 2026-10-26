/* Credential/session foundation. State transitions have no scheduling points;
 * all allocation, owner verification and output pinning precede publication.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/security.h>
#include <ciuki/boot_info.h>
extern struct ciuki_boot_info g_boot __attribute__((weak));

extern int security_owner_verify(const struct ciuki_security_profile *) __attribute__((weak));
extern int channel_peer_get(struct process *, int32_t, struct security_peer *) __attribute__((weak));
extern bool channel_peer_live(const struct security_peer *) __attribute__((weak));
extern void channel_peer_put(struct security_peer *) __attribute__((weak));
extern int proc_spawn_credential(uint32_t, uint32_t, uint32_t, uint32_t *) __attribute__((weak));

struct security_session {
    struct ciuki_credential credential;
    struct ciuki_security_profile profile;
    uint32_t state, refs;
    bool bootstrap;
};
struct security_subject {
    struct ciuki_credential credential;
    struct security_session *session;
    uint32_t refs;
    bool live;
};
_Static_assert(sizeof(struct security_subject) <= 2040, "subject fits kernel heap");
static struct security_session security_sessions[CIUKI_SECURITY_SESSION_MAX];
static struct security_subject system_subject;
static uint32_t session_high_water;

void security_init(struct process *p)
{
    system_subject = (struct security_subject){
        .credential = { .version = 1, .size = 64, .uid = CIUKI_UID_SYSTEM,
            .primary_gid = CIUKI_GID_SYSTEM, .group_count = 1 },
        .refs = 1, .live = true };
    p->subject = &system_subject;
}
const struct ciuki_credential *security_credential(const struct process *p)
{ return p && p->subject ? &p->subject->credential : 0; }
const struct ciuki_credential *security_subject_credential(const struct security_subject *s)
{ return s ? &s->credential : 0; }
bool security_system(const struct process *p)
{
    const struct ciuki_credential *c = security_credential(p);
    return c && c->uid == CIUKI_UID_SYSTEM && !c->session_id;
}
bool security_admin(const struct process *p)
{
    if (security_system(p)) return true;
    const struct ciuki_credential *c = security_credential(p);
    if (!c) return false;
    for (unsigned i = 0; i < c->group_count; i++)
        if (c->groups[i] == CIUKI_GID_ADMINISTRATORS) return true;
    return false;
}
bool security_subject_live(const struct security_subject *s)
{ return s && s->live && (!s->session || s->session->state == CIUKI_SECURITY_SESSION_ACTIVE); }
bool security_process_live(const struct process *p)
{ return p && security_subject_live(p->subject); }
bool security_subject_matches(const struct security_subject *s, const struct process *p)
{
    const struct ciuki_credential *c = security_credential(p);
    return s && c && !memcmp(&s->credential, c, sizeof(*c));
}
bool security_same(const struct process *a, const struct process *b)
{ return a && security_subject_matches(a->subject, b); }
int security_subject_get(struct security_subject *s)
{
    if (!s) return -EACCES;
    if (s->refs == UINT32_MAX) return -EAGAIN;
    s->refs++;
    return 0;
}
void security_subject_put(struct security_subject *s)
{
    if (!s || !s->refs || --s->refs) return;
    if (s->session) {
        struct security_session *session = s->session;
        session->refs--;
        if (!session->refs && session->state == CIUKI_SECURITY_SESSION_DEAD)
            memset(session, 0, sizeof(*session));
    }
    if (s != &system_subject) kfree(s);
}
int security_process_prepare(struct process *child, const struct process *parent)
{
    if (!security_process_live(parent)) return -EACCES;
    struct security_subject *s = kzalloc(sizeof(*s));
    if (!s) return -ENOMEM;
    if (!security_process_live(parent)) { kfree(s); return -EACCES; }
    if (parent->subject->session && parent->subject->session->refs == UINT32_MAX) {
        kfree(s);
        return -EAGAIN;
    }
    s->credential = *security_credential(parent);
    s->session = parent->subject->session;
    s->refs = 1;
    s->live = true;
    if (s->session) s->session->refs++;
    child->subject = s;
    return 0;
}
void security_process_stopping(struct process *p)
{ if (p->subject) p->subject->live = false; }
void security_process_release(struct process *p)
{
    struct security_subject *s = p->subject;
    if (s && !p->published && s->session && s->session->bootstrap && s->session->refs == 1 &&
        s->session->state == CIUKI_SECURITY_SESSION_ACTIVE)
        s->session->state = CIUKI_SECURITY_SESSION_DEAD;
    p->subject = 0;
    security_subject_put(s);
}
int security_query_process(struct process *caller, uint32_t pid, struct ciuki_credential *out)
{
    struct process *target = proc_find(pid);
    if (!target || target->state != PROC_LIVE) return -ESRCH;
    const struct ciuki_credential *a = security_credential(caller), *b = security_credential(target);
    if (!a || !b) return -EACCES;
    if (!security_system(caller) && (security_system(target) ||
        (!security_admin(caller) && (a->uid != b->uid || a->session_id != b->session_id))))
        return -EACCES;
    *out = *b;
    return 0;
}
static struct security_session *session_find(uint32_t id)
{
    for (unsigned i = 0; i < CIUKI_SECURITY_SESSION_MAX; i++)
        if (security_sessions[i].state && security_sessions[i].credential.session_id == id)
            return &security_sessions[i];
    return 0;
}
static int credential_template(const struct ciuki_credential *c)
{
    if (c->version != 1) return -EOPNOTSUPP;
    if (c->size != 64 || c->session_id || c->reserved ||
        !(c->flags & CIUKI_CRED_INTERACTIVE) ||
        (c->flags & ~(CIUKI_CRED_INTERACTIVE | CIUKI_CRED_SAFE_RECOVERY)) ||
        !c->group_count || c->group_count > 8) return -EINVAL;
    if ((c->flags & CIUKI_CRED_SAFE_RECOVERY) &&
        (c->uid != CIUKI_UID_ADMINISTRATOR || !&g_boot || !(g_boot.flags & CBI_F_SAFE_MODE))) return -EINVAL;
    for (unsigned i = c->group_count; i < 8; i++) if (c->groups[i]) return -EINVAL;
    bool ordinary = c->uid >= CIUKI_UID_FIRST_USER && c->uid <= CIUKI_UID_LAST_USER;
    if (c->uid == CIUKI_UID_GUEST)
        return c->primary_gid == 546 && c->group_count == 1 && c->groups[0] == 546 ? 0 : -EINVAL;
    if (!ordinary && c->uid != CIUKI_UID_ADMINISTRATOR) return -EINVAL;
    if (c->primary_gid != 545) return -EINVAL;
    if (c->group_count == 2 && c->groups[0] == 544 && c->groups[1] == 545) return 0;
    return ordinary && c->group_count == 1 && c->groups[0] == 545 ? 0 : -EINVAL;
}
static int profile_template(const struct ciuki_security_profile *p, uint32_t uid)
{
    if (p->version != 1) return -EOPNOTSUPP;
    if (p->size != 112 || p->uid != uid || p->flags || p->reserved ||
        !(p->volume_id_lo | p->volume_id_hi) ||
        !(p->owner_key_lo | p->owner_key_hi) ||
        !(p->owner_generation_lo | p->owner_generation_hi) ||
        !p->path_bytes || p->path_bytes > 64) return -EINVAL;
    for (unsigned i = 0; i < 64; i++) {
        if (i < p->path_bytes - 1) { if (!p->path[i]) return -EINVAL; }
        else if (p->path[i]) return -EINVAL;
    }
    /* k02 resolves retained G32 numeric roots as well as the new u<hex> form. */
    static const char prefix[] = "/Windows/Profiles/";
    unsigned n = sizeof(prefix) - 1;
    if (memcmp(p->path, prefix, n)) return -EINVAL;
    if (p->path[n] == 'u') {
        if (p->path_bytes != n + 10) return -EINVAL;
        for (unsigned i = n + 1; i < n + 9; i++)
            if (!((p->path[i] >= '0' && p->path[i] <= '9') ||
                  (p->path[i] >= 'a' && p->path[i] <= 'f'))) return -EINVAL;
    } else {
        if (p->path_bytes <= n + 1) return -EINVAL;
        for (unsigned i = n; i < p->path_bytes - 1; i++)
            if (p->path[i] < '0' || p->path[i] > '9') return -EINVAL;
    }
    return 0;
}
static int session_reserve(const struct ciuki_credential *c,
                           const struct ciuki_security_profile *p, uint32_t *id)
{
    struct security_session *slot = 0;
    for (unsigned i = 0; i < CIUKI_SECURITY_SESSION_MAX; i++) {
        struct security_session *s = &security_sessions[i];
        if (s->state == CIUKI_SECURITY_SESSION_ACTIVE || s->state == CIUKI_SECURITY_SESSION_CLOSING)
            return -EBUSY;
        if (!s->state) slot = s;
    }
    if (!slot || session_high_water == UINT32_MAX) return -EAGAIN;
    slot->credential = *c;
    slot->credential.session_id = ++session_high_water;
    slot->profile = *p;
    slot->bootstrap = false;
    slot->state = CIUKI_SECURITY_SESSION_ACTIVE;
    *id = session_high_water;
    return 0;
}
static int security_session_create_for(struct process *caller, const struct ciuki_credential *c,
                                       const struct ciuki_security_profile *p, uint32_t *id)
{
    int err = credential_template(c);
    if (!err) err = profile_template(p, c->uid);
    if (!err) err = security_owner_verify ? security_owner_verify(p) : -EIO;
    if (!err && caller && (!security_process_live(caller) || caller->state != PROC_LIVE)) err = -EACCES;
    return err ? err : session_reserve(c, p, id);
}
int security_session_create(const struct ciuki_credential *c,
                            const struct ciuki_security_profile *p, uint32_t *id)
{ return security_session_create_for(0, c, p, id); }
int security_session_attach(struct process *p, uint32_t id)
{
    struct security_session *s = session_find(id);
    if (!s || s->state != CIUKI_SECURITY_SESSION_ACTIVE) return -ESRCH;
    if (!p || p->published || p->state != PROC_PREPARING || !p->subject || p->subject->refs != 1 || p->threads)
        return -EINVAL;
    if (s->refs == UINT32_MAX) return -EAGAIN;
    if (p->subject->session) p->subject->session->refs--;
    p->subject->session = s;
    p->subject->credential = s->credential;
    s->refs++;
    /* Cross-credential cwd and descriptors are never inherited. */
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
        struct proc_object *o = p->fds[i].object;
        p->fds[i] = (struct proc_fd){ 0 };
        if (o) o->release(o);
    }
    void *cwd = p->cwd;
    p->cwd = 0;
    if (cwd && p->cwd_release) p->cwd_release(cwd);
    p->cwd_retain = 0;
    p->cwd_release = 0;
    return security_process_live(p) ? 0 : -EACCES;
}
int security_session_profile(uint32_t id, struct ciuki_security_profile *out)
{
    struct security_session *s = session_find(id);
    if (!s || s->state != CIUKI_SECURITY_SESSION_ACTIVE) return -ESRCH;
    *out = s->profile;
    return 0;
}
int security_session_query(struct process *caller, uint32_t id, uint32_t *state)
{
    if (!id || id > session_high_water) return -ESRCH;
    struct security_session *s = session_find(id);
    const struct ciuki_credential *c = security_credential(caller);
    if (!security_system(caller) && (!s || !c ||
        (!security_admin(caller) && c->session_id != id))) return -EACCES;
    *state = s ? s->state : CIUKI_SECURITY_SESSION_DEAD;
    return 0;
}
int security_session_end(uint32_t id, bool begin, uint32_t *state)
{
    if (!id || id > session_high_water) return -ESRCH;
    struct security_session *s = session_find(id);
    if (!s) { *state = CIUKI_SECURITY_SESSION_DEAD; return 0; }
    if (begin && s->state == CIUKI_SECURITY_SESSION_ACTIVE) {
        s->state = CIUKI_SECURITY_SESSION_CLOSING;
        for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) {
            struct process *p = proc_reserved_at_slot(i);
            if (p && p->subject && p->subject->session == s) proc_stop(p, 0, 0);
        }
        proc_notify();
    }
    if (s->state == CIUKI_SECURITY_SESSION_CLOSING) {
        bool draining = false;
        for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) {
            struct process *p = proc_reserved_at_slot(i);
            if (p && p->subject && p->subject->session == s && p->state != PROC_ZOMBIE)
                draining = true;
        }
        /* Unpublished children fence admission. Revoked subject snapshots
         * retain the DEAD record but cannot keep an old desktop executing. */
        if (!draining) s->state = CIUKI_SECURITY_SESSION_DEAD;
    }
    *state = s->state;
    if (s->state == CIUKI_SECURITY_SESSION_DEAD && !s->refs) memset(s, 0, sizeof(*s));
    return 0;
}

/* One interim interactive identity, minted internally, never a token setter.
 * Replaced by the trusted logon gate in sec-user-03. */
int security_bootstrap_desktop(struct process *p)
{
    if (!p || p->published || p->state != PROC_PREPARING || !p->subject || p->subject->refs != 1 || p->threads)
        return -EINVAL;
    for (unsigned i = 0; i < CIUKI_SECURITY_SESSION_MAX; i++)
        if (security_sessions[i].state == CIUKI_SECURITY_SESSION_ACTIVE && security_sessions[i].bootstrap)
            return security_session_attach(p, security_sessions[i].credential.session_id);
    struct ciuki_credential c = { .version = 1, .size = 64, .uid = 1000,
        .primary_gid = 545, .group_count = 2, .groups = {544, 545}, .flags = CIUKI_CRED_INTERACTIVE };
    struct ciuki_security_profile profile = { .version = 1, .size = 112, .uid = 1000 };
    uint32_t id;
    int err = session_reserve(&c, &profile, &id);
    if (!err) session_find(id)->bootstrap = true;
    return err ? err : security_session_attach(p, id);
}

static int security_span(uint32_t at, uint32_t bytes)
{
    if (!at || at > UINT32_MAX - bytes) return -EFAULT;
    return at & 3 ? -EINVAL : 0;
}
static bool security_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{ return a < b + bn && b < a + an; }
int security_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p) return -ENOSYS;
    if (tf->ecx != 64 || tf->edx || tf->esi || tf->edi || tf->ebp) return -EINVAL;
    int err = security_span(tf->ebx, 64);
    if (err) return err;
    struct ciuki_security_args a;
    if ((err = copy_from_user(&a, tf->ebx, 64))) return err;
    if (a.version != 1) return -EOPNOTSUPP;
    if (a.size != 64 || a.result || a.state || a.reserved[0] || a.reserved[1]) return -EINVAL;
    if (a.operation < 1 || a.operation > 7) return -EINVAL;
    bool query = a.operation <= CIUKI_SECURITY_QUERY_PEER;
    bool create = a.operation == CIUKI_SECURITY_SESSION_CREATE;
    bool spawn = a.operation == CIUKI_SECURITY_SPAWN_CREDENTIAL;
    bool end = a.operation == CIUKI_SECURITY_SESSION_END;
    if ((create || spawn || end) && !security_system(p)) return -EPERM;
    if (!security_process_live(p)) return -EACCES;
    if ((a.flags && !end) || (end && a.flags != CIUKI_SECURITY_END_BEGIN && a.flags != CIUKI_SECURITY_END_COLLECT) ||
        (!create && (a.credential_address || a.profile_address)) || (!spawn && a.spawn_address) ||
        (a.operation != CIUKI_SECURITY_QUERY_PROCESS && a.target_pid) ||
        (a.operation != CIUKI_SECURITY_QUERY_PEER && a.endpoint_fd) ||
        ((query || create) && a.session_id) || (!query && (a.output_address || a.output_bytes))) return -EINVAL;
    if (query && a.output_bytes != 64) return -EINVAL;
    if (a.operation == CIUKI_SECURITY_QUERY_PROCESS && (!a.target_pid || a.target_pid > INT32_MAX)) return -EINVAL;
    if (a.operation == CIUKI_SECURITY_QUERY_PEER && (a.endpoint_fd < 0 || a.endpoint_fd >= CIUKI_OPEN_MAX)) return -EBADF;
    if (end && !a.session_id) return -EINVAL;
    struct ciuki_credential credential = {0};
    struct ciuki_security_profile profile;
    if (create) {
        if ((err = security_span(a.credential_address, 64)) || (err = security_span(a.profile_address, 112))) return err;
        if (security_overlap(tf->ebx, 64, a.credential_address, 64) ||
            security_overlap(tf->ebx, 64, a.profile_address, 112) ||
            security_overlap(a.credential_address, 64, a.profile_address, 112)) return -EINVAL;
        if ((err = copy_from_user(&credential, a.credential_address, 64)) ||
            (err = copy_from_user(&profile, a.profile_address, 112))) return err;
    }
    if (query) {
        if ((err = security_span(a.output_address, 64))) return err;
        if (security_overlap(tf->ebx, 64, a.output_address, 64)) return -EINVAL;
    }
    struct ua_pin args_pin = {0}, out_pin = {0};
    if ((err = ua_pin(p->memory, &args_pin, tf->ebx, 64, true))) return err;
    if (query && (err = ua_pin(p->memory, &out_pin, a.output_address, 64, true))) goto done;
    if (!security_process_live(p)) { err = -EACCES; goto done; }
    switch (a.operation) {
    case CIUKI_SECURITY_QUERY_SELF: credential = *security_credential(p); break;
    case CIUKI_SECURITY_QUERY_PROCESS: err = security_query_process(p, a.target_pid, &credential); break;
    case CIUKI_SECURITY_QUERY_PEER: {
        struct security_peer peer = {0};
        err = channel_peer_get ? channel_peer_get(p, a.endpoint_fd, &peer) : -EBADF;
        if (!err) {
            if (!channel_peer_live(&peer)) err = -ESRCH;
            else credential = *security_subject_credential(peer.subject);
            channel_peer_put(&peer);
        }
        break;
    }
    case CIUKI_SECURITY_SESSION_CREATE: err = security_session_create_for(p, &credential, &profile, &a.result); break;
    case CIUKI_SECURITY_SPAWN_CREDENTIAL:
        err = proc_spawn_credential ? proc_spawn_credential(a.spawn_address, a.session_id, tf->ebx, &a.result) : -ENOSYS;
        break;
    case CIUKI_SECURITY_SESSION_END:
        err = security_session_end(a.session_id, a.flags == CIUKI_SECURITY_END_BEGIN, &a.state); break;
    case CIUKI_SECURITY_SESSION_QUERY: err = security_session_query(p, a.session_id, &a.state); break;
    }
    if (!err) {
        if (query) err = copy_to_user(a.output_address, &credential, 64);
        if (!err) err = copy_to_user(tf->ebx, &a, 64);
    }
done:
    if (query) ua_unpin(p->memory, &out_pin);
    ua_unpin(p->memory, &args_pin);
    return err;
}
