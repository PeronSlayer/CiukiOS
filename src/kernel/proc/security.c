/* Credential/session foundation. State transitions have no scheduling points;
 * all allocation, owner verification and output pinning precede publication.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/security.h>
#include <ciuki/boot_info.h>
#include <ciuki/signal.h>
#include <ciuki/setup_mode.h>
#pragma weak desktop_close
#include <ciuki/desktop.h>
extern struct ciuki_boot_info g_boot __attribute__((weak));

extern int security_owner_verify(const struct ciuki_security_profile *) __attribute__((weak));
extern void channel_fd_closed(struct process *,struct proc_object *) __attribute__((weak));
extern int channel_peer_get(struct process *, int32_t, struct security_peer *) __attribute__((weak));
extern bool channel_peer_live(const struct security_peer *) __attribute__((weak));
extern void channel_peer_put(struct security_peer *) __attribute__((weak));
extern int proc_spawn_credential(uint32_t, uint32_t, uint32_t, uint32_t *) __attribute__((weak));

struct security_session {
    struct ciuki_credential credential;
    struct ciuki_security_profile profile;
    uint32_t state, refs;
    bool bootstrap, initial_activation, constructing;
    uint32_t generation;
    struct ciuki_security_console console;
    struct ciuki_security_launch launches[2];
    uint64_t deadlines[2];
    struct proc_object *control_objects[2];
    struct security_subject *controllers[2];
    struct proc_object *child_objects[2][5];
    bool fencing, fence_pending;
};
struct security_subject {
    struct ciuki_credential credential;
    struct security_session *session;
    uint32_t refs;
    bool live;
    uint32_t pid, process_generation, role, role_generation;
    struct security_peer peer; /* only endpoint-acquired worker leases */
    struct security_subject *origin;
};
_Static_assert(sizeof(struct security_subject) <= 2040, "subject fits kernel heap");
static struct security_session security_sessions[CIUKI_SECURITY_SESSION_MAX];
static struct security_subject system_subject;
static struct proc_fd owner_endpoint_fds[CIUKI_OPEN_MAX];
static uint32_t session_high_water, process_high_water = 1, launch_high_water;
static uint32_t role_generations[9];
static const struct security_grant_ops *security_grants;
static struct ciuki_security_console boot_console = { .version=1, .size=64,
    .console_generation=1, .grant_generation=1, .state=CIUKI_SECURITY_CONSOLE_TRUSTED,
    .console_epoch=1, .authentication_epoch=1 };
static uint64_t cookie_high_water;
static uint32_t console_logon_generation;
static void security_role_stopping(struct process *);
static void session_conceal(struct security_session *);
static void session_controls_close(struct security_session *);
static void launch_snapshot(struct security_session *, unsigned, struct ciuki_security_launch *);
extern void channel_security_poll(void) __attribute__((weak));
extern uint32_t channel_peer_service(const struct security_peer *) __attribute__((weak));
extern int channel_security_link(struct process *, uint32_t, const struct ciuki_security_link *,
    struct ciuki_security_link *, uint32_t *) __attribute__((weak));
extern int channel_kernel_receive(struct process *, int32_t, struct ciuki_message *) __attribute__((weak));
extern int channel_control_install(struct process *,struct proc_object *) __attribute__((weak));
extern void channel_security_revoke(struct process *) __attribute__((weak));
extern int proc_security_launch_prepare(struct process *, struct ciuki_security_launch *, struct process **) __attribute__((weak));
/* sec-k07: the real device backend and the session drain, when linked. */
#pragma weak secure_console_ops
#pragma weak secure_session_retained

void security_init(struct process *p)
{
    system_subject = (struct security_subject){
        .credential = { .version = 1, .size = 64, .uid = CIUKI_UID_SYSTEM,
            .primary_gid = CIUKI_GID_SYSTEM, .group_count = 1 },
        .refs = 1, .live = true, .pid=p->pid, .process_generation=1,
        .role=CIUKI_SECURITY_ROLE_OWNER, .role_generation=1 };
    role_generations[CIUKI_SECURITY_ROLE_OWNER]=1;
    p->fds=owner_endpoint_fds;
    p->subject = &system_subject;
    if (&secure_console_ops) security_grants=&secure_console_ops; /* sec-k07 */
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
{ return s && s->live && (!s->session || s->session->state == CIUKI_SECURITY_SESSION_ACTIVE) &&
    (!s->origin || (security_subject_live(s->origin) && channel_peer_live && channel_peer_live(&s->peer))); }
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
    if (s->origin) {
        channel_peer_put(&s->peer);
        security_subject_put(s->origin);
    }
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
    if (process_high_water == UINT32_MAX) { kfree(s); return -EAGAIN; }
    s->pid = child->pid;
    s->process_generation = ++process_high_water;
    s->credential = *security_credential(parent);
    s->session = parent->subject->session;
    s->refs = 1;
    s->live = true;
    if (s->session) s->session->refs++;
    child->subject = s;
    return 0;
}
void security_process_stopping(struct process *p)
{
    if (!p->subject || !p->subject->live) return;
    p->subject->live = false;
    security_role_stopping(p);
    if (channel_security_revoke) channel_security_revoke(p);
}
void security_process_release(struct process *p)
{
    struct security_subject *s = p->subject;
    if (s && !p->published && s->session && s->session->bootstrap && s->session->refs == 1 &&
        s->session->state == CIUKI_SECURITY_SESSION_ACTIVE)
        s->session->state = CIUKI_SECURITY_SESSION_DEAD;
    if (s && s->role && !p->published) security_role_stopping(p);
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
    slot->generation = 1;
    slot->console = boot_console;
    slot->console.session_id = session_high_water;
    slot->console.session_generation = 1;
    slot->console.authentication_required = 0;
    slot->initial_activation = true;
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
        if (o) {
            if (channel_fd_closed) channel_fd_closed(p,o);
            o->release(o);
        }
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
        s->initial_activation = false;
        s->console.restore_cookie_lo = s->console.restore_cookie_hi = 0;
        if (s->generation != UINT32_MAX) s->generation++;
        session_conceal(s);
        session_controls_close(s);
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
        /* sec-k07: DOS VMs (kernel tasks, not processes) and Win32 session
         * objects/reports are drained by PID 1; nonblocking query here. */
        if (secure_session_retained && secure_session_retained(id)) draining = true;
        /* Unpublished children fence admission. Revoked subject snapshots
         * retain the DEAD record but cannot keep an old desktop executing. */
        if (!draining && !s->fence_pending && !s->fencing && !s->constructing) {
            s->state = CIUKI_SECURITY_SESSION_DEAD;
            boot_console = s->console;
            boot_console.session_id = boot_console.session_generation = 0;
            boot_console.authentication_required = 0;
            boot_console.restore_cookie_lo = boot_console.restore_cookie_hi = 0;
            /* sec-k07: the backend releases kernel-held endpoints whose last
             * subject reference may be this session's; keep the record alive
             * until its state has been reported. */
            if (security_grants && security_grants->release && s->refs != UINT32_MAX) {
                s->refs++;
                security_grants->release(id);
                s->refs--;
            }
        }
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
    /* sec-own: once OWNER has committed uid 1000's profile (CiukiUser on a
     * ready image, Setup's first administrator), the interim session carries
     * that committed descriptor; until then it has none (k05 legacy stores). */
    extern int owner_system_profile(uint32_t, struct ciuki_security_profile *) __attribute__((weak));
    struct ciuki_security_profile committed;
    if (owner_system_profile && !owner_system_profile(1000, &committed) &&
        !profile_template(&committed, 1000) && security_owner_verify && !security_owner_verify(&committed)) profile = committed;
    /* end sec-own */
    uint32_t id;
    int err = session_reserve(&c, &profile, &id);
    if (!err) session_find(id)->bootstrap = true;
    return err ? err : security_session_attach(p, id);
}

/* Role metadata is assigned only by fixed internal preparation paths. Public
 * spawn inherits the credential, never role or service generations. */
uint32_t security_role(const struct process *p) { return p && p->subject ? p->subject->role : 0; }
uint32_t security_process_generation(const struct process *p) { return p && p->subject ? p->subject->process_generation : 0; }
uint32_t security_role_generation(const struct process *p) { return p && p->subject ? p->subject->role_generation : 0; }
uint32_t security_subject_role(const struct security_subject *s) { return s ? s->role : 0; }
uint32_t security_subject_pid(const struct security_subject *s) { return s ? s->pid : 0; }
uint32_t security_subject_process_generation(const struct security_subject *s) { return s ? s->process_generation : 0; }
bool security_logon(const struct process *p)
{ return security_system(p) && security_role(p) == CIUKI_SECURITY_ROLE_LOGON && security_process_live(p); }
const char *security_role_image(uint32_t role)
{
    static const char *const paths[] = {0, "/System/Services/logon", "/System/Services/logon-ui",
        "/System/Services/accounts", 0, "/System/Services/registry", "/Programs/desktop",
        "/System/Win32/w32srv", "/Programs/setup"};
    return role < ARRAY_SIZE(paths) ? paths[role] : 0;
}
static struct process *role_process(uint32_t role, uint32_t session, bool preparing)
{
    for (unsigned i=1; i<CIUKI_PROCESS_MAX; i++) {
        struct process *p=proc_reserved_at_slot(i);
        const struct ciuki_credential *c=security_credential(p);
        if (c && c->session_id==session && security_role(p)==role && security_process_live(p) &&
            (p->state==PROC_LIVE || (preparing && p->state==PROC_PREPARING))) return p;
    }
    return 0;
}
int security_role_prepare(struct process *parent, struct process *child, uint32_t role, const char *image)
{
    const char *fixed=security_role_image(role);
    if (!fixed || !image || strncmp(image,fixed,strlen(fixed)+1)) return -EACCES;
    if (!child || child->published || child->state!=PROC_PREPARING || !child->subject ||
        child->subject->role || child->subject->refs!=1) return -EINVAL;
    const struct ciuki_credential *c=security_credential(child);
    bool user=role==CIUKI_SECURITY_ROLE_DESKTOP || role==CIUKI_SECURITY_ROLE_W32SRV;
    if (user) {
        if (!security_logon(parent) || !c->session_id) return -EPERM;
    } else {
        if (parent!=proc_supervisor() || child->ppid!=parent->pid || !security_system(child)) return -EPERM;
        if (role==CIUKI_SECURITY_ROLE_SETUP && (!&g_boot || !setup_mode_parse(g_boot.flags,g_boot.options,sizeof(g_boot.options)))) return -EPERM;
    }
    if (role_process(role,c->session_id,true)) return -EBUSY;
    if (role_generations[role]==UINT32_MAX) return -EAGAIN;
    child->subject->role=role;
    child->subject->role_generation=++role_generations[role];
    return 0;
}
static uint32_t service_role(uint32_t service)
{
    static const uint32_t roles[]={0,3,3,4,5,1,7};
    return service<ARRAY_SIZE(roles) ? roles[service] : 0;
}
struct process *security_service_server(uint32_t service, uint32_t session)
{
    if (service==CIUKI_SECURITY_SERVICE_OWNER) return proc_supervisor();
    uint32_t role=service_role(service);
    return role ? role_process(role,service==6 ? session : 0,false) : 0;
}
struct process *security_session_desktop(uint32_t session)
{ return session ? role_process(CIUKI_SECURITY_ROLE_DESKTOP,session,false) : 0; }
bool security_roles_active(void) { return role_generations[CIUKI_SECURITY_ROLE_LOGON]!=0; }
int security_service_allowed(const struct process *p, uint32_t service)
{
    if (!security_process_live(p)) return -EACCES;
    uint32_t r=security_role(p);
    switch (service) {
    case 1: return security_system(p) && (r==1 || r==2) ? 0 : -EPERM;
    case 2: case 4: return 0;
    case 3: return security_system(p) && (r==3 || r==8) ? 0 : -EPERM;
    case 5: return (security_system(p) && (r==2 || r==3)) ||
        (r==6 && security_credential(p)->session_id) ? 0 : -EPERM;
    case 6: return security_credential(p)->session_id ? 0 : -EACCES;
    default: return -EINVAL;
    }
}
int security_subject_acquire_endpoint(struct process *p, int32_t fd, uint32_t generation,
                                      struct security_subject **out)
{
    if (!out) return -EINVAL;
    struct security_subject *s=kzalloc(sizeof(*s));
    if (!s) return -ENOMEM;
    int err=channel_peer_get ? channel_peer_get(p,fd,&s->peer) : -EBADF;
    if (!err && generation!=s->peer.generation) err=-EAGAIN;
    if (!err && !channel_peer_live(&s->peer)) err=-ESRCH;
    if (!err) err=security_subject_get(s->peer.subject);
    if (err) { if (s->peer.endpoint) channel_peer_put(&s->peer); kfree(s); return err; }
    struct security_subject *origin=s->peer.subject;
    s->credential=origin->credential;
    s->pid=origin->pid; s->process_generation=origin->process_generation;
    s->role=origin->role; s->role_generation=origin->role_generation;
    s->origin=origin; s->refs=1; s->live=true;
    *out=s;
    return 0;
}
int security_subject_same_link(const struct security_subject *a, const struct security_subject *b)
{
    if (!a || !b || !a->origin || !b->origin || !a->peer.endpoint || !b->peer.endpoint)
        return -EACCES;
    /* The leases pin both endpoint and origin, so neither identity can be
     * recycled beneath this comparison. No allocation, wait or callback. */
    if (!security_subject_live(a) || !security_subject_live(b)) return -ESRCH;
    return a->origin == b->origin && a->pid == b->pid &&
        a->process_generation == b->process_generation &&
        a->role_generation == b->role_generation &&
        a->peer.endpoint == b->peer.endpoint && a->peer.subject == b->peer.subject &&
        a->peer.generation == b->peer.generation;
}
int security_subject_check(const struct security_subject *s, uint32_t action, uint32_t scope)
{
    if (!s) return -EACCES;
    if (!security_subject_live(s)) return -ESRCH;
    if (scope && s->credential.session_id!=scope) return -EACCES;
    uint32_t service=action>>16, op=action&65535u, r=s->role;
    if (!service) return action ? -EINVAL : 0;
    if (!s->origin || !channel_peer_service || channel_peer_service(&s->peer)!=service) return -EACCES;
    /* sec-own: OWNER query operations 8..14 (fs/owner_system.h). */
    if (!op || (service==2 && op>9) || (service==3 && op>14)) return -EOPNOTSUPP;
    if (service==1) return r==1 ? (op>=1 && op<=7 ? 0 : -EOPNOTSUPP) :
        r==2 && op==1 ? 0 : -EPERM;
    if (service==5) {
        if ((r==3 && op==1) || (r==2 && (op==2 || op==3 || op==4 || op==6 || op==7)) ||
            (r==6 && op==5 && scope && s->credential.session_id==scope)) return 0;
        return -EPERM;
    }
    if (service==3 && r!=3 && r!=8) return -EPERM;
    return service>=2 && service<=6 ? 0 : -EINVAL;
}
uint32_t security_session_generation(uint32_t id)
{ struct security_session *s=session_find(id); return s ? s->generation : 0; }
void security_set_grant_ops(const struct security_grant_ops *ops) { security_grants=ops; }
int security_trusted_prepare(struct process *p)
{
    if (security_role(p)!=CIUKI_SECURITY_ROLE_LOGON_UI) return -EPERM;
    int err=security_grants && security_grants->trusted ? security_grants->trusted(p) : -EIO;
    if (!err) for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) if (p->fds[i].object) {
        p->fds[i].object->inheritable=false;
        p->fds[i].flags|=FD_CLOEXEC;
    }
    return err;
}
static int console_advance(struct ciuki_security_console *c, bool enter)
{
    if (c->console_epoch==UINT32_MAX || c->console_generation==UINT32_MAX ||
        c->grant_generation==UINT32_MAX || (enter && c->authentication_epoch==UINT32_MAX)) return -EAGAIN;
    c->console_epoch++; c->console_generation++; c->grant_generation++;
    if (enter) c->authentication_epoch++;
    return 0;
}
static void session_conceal(struct security_session *s)
{
    uint32_t flags=irq_save();
    s->console.state=CIUKI_SECURITY_CONSOLE_TRUSTED;
    s->initial_activation=false;
    s->console.restore_cookie_lo=s->console.restore_cookie_hi=0;
    /* An unpublished/no-grant session has nothing to fence. Keep a failed
     * device fence in CLOSING; a later logon must not reuse that console. */
    if (!s->launches[0].state && !s->launches[1].state && !s->constructing) { irq_restore(flags); return; }
    s->fence_pending=true;
    if (s->fencing) { irq_restore(flags); return; }
    s->fencing=true; s->refs++;
    int err=security_grants && security_grants->conceal ?
        security_grants->conceal(s->credential.session_id,s->console.grant_generation) : -EIO;
    s->refs--; s->fencing=false;
    if (!err) s->fence_pending=false;
    irq_restore(flags);
}
static void security_role_stopping(struct process *p)
{
    uint32_t role=security_role(p);
    if (!role) return;
    /* A failed private preparation retires its reservation, not a live gate. */
    if (!p->published) { p->subject->role=0; return; }
    if (role_generations[role]!=UINT32_MAX) role_generations[role]++;
    if (role==1 || role==2 || role==3) {
        for (unsigned i=0;i<CIUKI_SECURITY_SESSION_MAX;i++) {
            struct security_session *s=&security_sessions[i];
            if (s->state!=CIUKI_SECURITY_SESSION_ACTIVE) continue;
            session_conceal(s);
            s->console.authentication_required=1;
            (void)console_advance(&s->console,true);
            s->console.restore_cookie_lo=s->console.restore_cookie_hi=0;
            if (s->generation!=UINT32_MAX) s->generation++;
        }
        (void)console_advance(&boot_console,true);
        boot_console.restore_cookie_lo=boot_console.restore_cookie_hi=0;
        console_logon_generation=0;
    }
    if (role==6 || role==7) {
        struct security_session *s=p->subject->session;
        if (s && s->state==CIUKI_SECURITY_SESSION_ACTIVE) {
            s->state=CIUKI_SECURITY_SESSION_CLOSING;
            session_conceal(s);
            for (unsigned i=0;i<2;i++) if (s->launches[i].pid==p->pid) {
                s->launches[i].state=CIUKI_SECURITY_LAUNCH_FAILED;
                s->launches[i].error=-ESRCH;
            }
            session_controls_close(s);
            if (s->generation!=UINT32_MAX) s->generation++;
            /* Stop the paired process without recursing into ACTIVE again. */
            for (unsigned i=1;i<CIUKI_PROCESS_MAX;i++) {
                struct process *other=proc_reserved_at_slot(i);
                if (other && other!=p && other->subject && other->subject->session==s) proc_stop(other,0,0);
            }
        }
    }
}
static int console_enter_locked(struct process *logon, uint32_t id, const struct ciuki_security_console *in,
                         struct ciuki_security_console *out, bool physical)
{
    struct security_session *s=id ? session_find(id) : 0;
    if (!id) {
        for (unsigned i=0;i<CIUKI_SECURITY_SESSION_MAX;i++)
            if (security_sessions[i].state==CIUKI_SECURITY_SESSION_ACTIVE ||
                security_sessions[i].state==CIUKI_SECURITY_SESSION_CLOSING) return -EBUSY;
        if (in->session_generation || in->reason!=4) return -EINVAL;
    } else if (!s || s->state!=CIUKI_SECURITY_SESSION_ACTIVE) return -ESRCH;
    else if (in->session_generation!=s->generation) return -EAGAIN;
    struct ciuki_security_console *c=s ? &s->console : &boot_console;
    if (s && s->fencing) return -EBUSY;
    uint32_t binding=security_role_generation(logon);
    uint32_t required=c->authentication_required || (in->reason!=4);
    if (!physical && c->state==1 && c->reason==in->reason && required==c->authentication_required &&
        binding && console_logon_generation==binding && (c->restore_cookie_lo || c->restore_cookie_hi)) {
        *out=*c; out->session_generation=s ? s->generation : 0; return 0;
    }
    /* Conceal first, including exhaustion/fence failure; never expose on error. */
    c->state=1; c->authentication_required=required;
    c->restore_cookie_lo=c->restore_cookie_hi=0;
    if (s) s->initial_activation=false;
    uint32_t epoch_before=c->console_epoch, auth_before=c->authentication_epoch;
    if (s) { s->refs++; s->fencing=true; s->fence_pending=true; }
    int fence=security_grants && security_grants->conceal ?
        security_grants->conceal(id,c->grant_generation) : -EIO;
    if (s) { s->refs--; s->fencing=false; if (!fence) s->fence_pending=false; }
    if ((logon && (!security_logon(logon) || logon->state!=PROC_LIVE ||
         security_role_generation(logon)!=binding)) ||
        (s && s->state!=CIUKI_SECURITY_SESSION_ACTIVE) ||
        c->console_epoch!=epoch_before || c->authentication_epoch!=auth_before) return -EAGAIN;
    c->restore_cookie_lo=c->restore_cookie_hi=0;
    int err=(s && s->generation==UINT32_MAX) || cookie_high_water==UINT64_MAX ?
        -EAGAIN : console_advance(c,true);
    if (err) return err;
    if (s) s->generation++;
    c->reason=in->reason;
    c->session_generation=s ? s->generation : 0;
    console_logon_generation=binding;
    if (fence) return fence;
    uint64_t cookie=++cookie_high_water;
    c->restore_cookie_lo=(uint32_t)cookie; c->restore_cookie_hi=(uint32_t)(cookie>>32);
    *out=*c;
    return 0;
}
static int console_enter(struct process *logon, uint32_t id, const struct ciuki_security_console *in,
                         struct ciuki_security_console *out, bool physical)
{
    uint32_t flags=irq_save();
    int err=console_enter_locked(logon,id,in,out,physical);
    irq_restore(flags); return err;
}
static bool launch_pair_ready(struct security_session *s)
{ return s->launches[0].state>=2 && s->launches[0].state<=3 && s->launches[1].state>=2 && s->launches[1].state<=3; }
static int console_activate(struct security_session *s)
{
    if (!launch_pair_ready(s)) return -EBUSY;
    if (s->fencing || s->fence_pending) return -EBUSY;
    if (s->generation==UINT32_MAX || s->console.console_epoch==UINT32_MAX ||
        s->console.console_generation==UINT32_MAX || s->console.grant_generation==UINT32_MAX) return -EAGAIN;
    if (!security_grants || !security_grants->activate) return -EIO;
    int err=security_grants->activate(s->credential.session_id,s->console.grant_generation+1);
    if (err) { session_conceal(s); return err; }
    /* activate is nonblocking; this commit interval cannot yield. */
    (void)console_advance(&s->console,false);
    s->generation++;
    s->console.session_generation=s->generation;
    s->console.state=2; s->console.authentication_required=0;
    s->console.restore_cookie_lo=s->console.restore_cookie_hi=0;
    s->initial_activation=false;
    s->launches[0].state=s->launches[1].state=3;
    return 0;
}
int security_console_attention(uint32_t reason)
{
    if (reason!=CIUKI_SECURITY_SECURE_ATTENTION && reason!=CIUKI_SECURITY_SECURE_RESUME) return -EINVAL;
    struct process *logon=role_process(1,0,false);
    for (unsigned i=0;i<CIUKI_SECURITY_SESSION_MAX;i++) {
        struct security_session *s=&security_sessions[i];
        if (s->state!=1) continue;
        struct ciuki_security_console in={.reason=reason,.session_generation=s->generation}, out;
        int err=console_enter(logon,s->credential.session_id,&in,&out,true);
        if (logon) kwait_wake_all(&logon->changed);
        proc_notify();
        return err;
    }
    struct ciuki_security_console in={.reason=CIUKI_SECURITY_SECURE_ATTENTION},out;
    int err=console_enter(logon,0,&in,&out,true);
    if (logon) kwait_wake_all(&logon->changed);
    proc_notify();
    return err;
}
static int console_leave_locked(struct process *p, uint32_t id, const struct ciuki_security_console *in,
                         struct ciuki_security_console *out)
{
    if (!id) return -EACCES;
    struct security_session *s=session_find(id);
    if (!s || s->state!=1) return -ESRCH;
    if (s->console.state!=CIUKI_SECURITY_CONSOLE_TRUSTED) return -EAGAIN;
    struct ciuki_security_console expected=s->console;
    expected.session_generation=s->generation; expected.state=0;
    if (console_logon_generation!=security_role_generation(p) ||
        !(in->restore_cookie_lo|in->restore_cookie_hi) || memcmp(in,&expected,sizeof(*in))) return -EAGAIN;
    /* The registered LOGON alone consumes the matching AUTH_OUTCOME. Neither
     * desktop nor UI has a syscall field that can manufacture that proof. */
    int err=console_activate(s);
    if (!err) *out=s->console;
    return err;
}
static int console_leave(struct process *p, uint32_t id, const struct ciuki_security_console *in,
                         struct ciuki_security_console *out)
{
    uint32_t flags=irq_save();
    int err=console_leave_locked(p,id,in,out);
    irq_restore(flags); return err;
}
static void launch_fds_clear(struct ciuki_security_launch *l)
{
    l->control_fd=l->child_ready_fd=l->child_console_fd=l->child_display_fd=l->child_input_fd=l->child_service_fd=-1;
}
static void session_controls_close(struct security_session *s)
{
    for (unsigned i=0;i<2;i++) {
        struct proc_object *o=s->control_objects[i];
        struct security_subject *controller=s->controllers[i];
        s->control_objects[i]=0; s->controllers[i]=0;
        for (unsigned j=0;j<5;j++) {
            struct proc_object *child=s->child_objects[i][j];
            s->child_objects[i][j]=0;
            if (child) child->release(child);
        }
        if (!o) { launch_fds_clear(&s->launches[i]); continue; }
        struct process *p=proc_find(security_subject_pid(controller));
        int32_t fd=s->launches[i].control_fd;
        if (p && p->subject==controller && p->fds && fd>=0 && fd<CIUKI_OPEN_MAX && p->fds[fd].object==o && desktop_close)
            desktop_close(p,fd);
        o->release(o);
        security_subject_put(controller);
        launch_fds_clear(&s->launches[i]);
    }
}
static void launch_failed(struct security_session *s, unsigned which, int error)
{
    struct ciuki_security_launch *l=&s->launches[which];
    l->state=4; l->error=error;
    launch_fds_clear(l);
    uint32_t state;
    (void)security_session_end(s->credential.session_id,true,&state);
}
void security_tick(void)
{
    /* The reaper otherwise sleeps when no process exits; readiness and
     * reassembly deadlines must not depend on a cooperative LOGON poll. */
    if (g_ticks%100u==0) proc_notify();
}
void security_poll(void)
{
    if (channel_security_poll) channel_security_poll();
    for (unsigned i=0;i<CIUKI_SECURITY_SESSION_MAX;i++) {
        struct security_session *s=&security_sessions[i];
        if (s->state==CIUKI_SECURITY_SESSION_CLOSING && s->fence_pending) session_conceal(s);
        if (s->state!=1 || s->constructing) continue;
        for (unsigned j=0;j<2;j++) {
            struct ciuki_security_launch *l=&s->launches[j];
            if (!l->state || l->state==4) continue;
            struct process *p=proc_find(l->pid);
            if (!p || p->state!=PROC_LIVE || security_process_generation(p)!=l->process_generation) {
                launch_failed(s,j,-ESRCH); break;
            }
            if (l->state==1 && g_ticks>=s->deadlines[j]) { launch_failed(s,j,-ETIMEDOUT); break; }
        }
    }
}
int security_launch_resources(struct process *p, struct ciuki_security_launch *l)
{
    if (!security_grants || !security_grants->prepare) return -EIO;
    return security_grants->prepare(p,l->purpose,l);
}
void security_launch_preparation_done(uint32_t id)
{
    struct security_session *s=session_find(id);
    if (!s || !s->constructing) return;
    s->constructing=false; s->refs--;
    if (!s->refs && s->state==CIUKI_SECURITY_SESSION_DEAD) memset(s,0,sizeof(*s));
}
static int session_launch(struct process *parent, const struct ciuki_security_launch *in,
                          struct ciuki_security_launch *out, bool poll)
{
    struct security_session *s=session_find(in->session_id);
    if (!s || (s->state!=1 && !(poll && s->state==CIUKI_SECURITY_SESSION_CLOSING))) return -ESRCH;
    if (in->session_generation!=s->generation) return -EAGAIN;
    if (in->purpose<1 || in->purpose>2) return -EINVAL;
    unsigned which=in->purpose-1;
    struct ciuki_security_launch *l=&s->launches[which];
    if (poll) {
        if (!l->state || l->launch_id!=in->launch_id || l->launch_generation!=in->launch_generation) return -EAGAIN;
        if (s->control_objects[which] && s->controllers[which]!=parent->subject &&
            !security_subject_live(s->controllers[which])) {
            int err=security_subject_get(parent->subject);
            if (err) return err;
            int fd=channel_control_install ? channel_control_install(parent,s->control_objects[which]) : -EIO;
            if (fd<0) { security_subject_put(parent->subject); return fd; }
            security_subject_put(s->controllers[which]);
            s->controllers[which]=parent->subject; l->control_fd=fd;
        }
        security_poll();
        if (l->state==1) {
            struct ciuki_message ready;
            bool original=s->controllers[which]==parent->subject && l->control_fd>=0 &&
                parent->fds[l->control_fd].object==s->control_objects[which];
            int err=!original ? -ESRCH : channel_kernel_receive ? channel_kernel_receive(parent,l->control_fd,&ready) : -EIO;
            if (err==-EAGAIN) { /* no handshake yet */ }
            else if (err!=1) launch_failed(s,which,err<0 ? err : -ESRCH);
            else {
                uint32_t words[4]; memcpy(words,ready.data,16);
                if (ready.length!=16 || ready.fd_count || words[0]!=1 || words[1]!=l->launch_id ||
                    words[2]!=l->launch_generation || (int32_t)words[3]>0 || ready.sender_pid!=l->pid)
                    launch_failed(s,which,-EINVAL);
                else if ((int32_t)words[3]<0) launch_failed(s,which,(int32_t)words[3]);
                else l->state=2;
            }
        }
        uint32_t flags=irq_save();
        if (which==0 && launch_pair_ready(s) && s->initial_activation && !s->console.authentication_required &&
            l->activation_console_epoch==s->console.console_epoch &&
            l->activation_authentication_epoch==s->console.authentication_epoch) {
            int err=console_activate(s);
            if (err) { irq_restore(flags); return err; }
        }
        irq_restore(flags);
        launch_snapshot(s,which,out); out->session_generation=s->generation;
        out->grant_generation=s->console.grant_generation;
        return 0;
    }
    if (l->state || s->constructing) return -EBUSY;
    if (launch_high_water==UINT32_MAX || s->generation==UINT32_MAX) return -EAGAIN;
    struct ciuki_security_launch next=*in;
    launch_fds_clear(&next);
    next.launch_id=++launch_high_water; next.launch_generation=next.launch_id;
    next.state=1; next.grant_generation=s->console.grant_generation;
    next.session_generation=s->generation+1;
    next.activation_console_epoch=s->console.console_epoch;
    next.activation_authentication_epoch=s->console.authentication_epoch;
    s->constructing=true; s->refs++;
    struct process *child=0;
    int err=proc_security_launch_prepare ? proc_security_launch_prepare(parent,&next,&child) : -EIO;
    security_launch_preparation_done(in->session_id);
    if (!err && (!security_logon(parent) || parent->state!=PROC_LIVE ||
        !security_process_live(child) || s->state!=1 || in->session_generation!=s->generation)) err=-EAGAIN;
    if (err) {
        if (child) proc_discard(child);
        if (next.control_fd>=0) desktop_close(parent,next.control_fd);
        if (!s->refs && s->state==CIUKI_SECURITY_SESSION_DEAD) memset(s,0,sizeof(*s));
        return err;
    }
    int retain=security_subject_get(parent->subject);
    if (retain) { proc_discard(child); desktop_close(parent,next.control_fd); return retain; }
    s->controllers[which]=parent->subject;
    s->control_objects[which]=parent->fds[next.control_fd].object;
    s->control_objects[which]->retain(s->control_objects[which]);
    int32_t child_fds[]={next.child_ready_fd,next.child_console_fd,next.child_display_fd,
        next.child_input_fd,next.child_service_fd};
    for (unsigned i=0;i<ARRAY_SIZE(child_fds);i++) if (child_fds[i]>=0) {
        struct proc_object *o=child->fds[child_fds[i]].object;
        s->child_objects[which][i]=o; o->retain(o);
    }
    next.pid=child->pid; next.process_generation=security_process_generation(child);
    next.service_generation=security_role_generation(child);
    next.session_generation=++s->generation;
    *l=next;
    s->deadlines[which]=g_ticks+CIUKI_SECURITY_LAUNCH_TIMEOUT_MS;
    proc_publish(child,CIUKI_SPAWN_NEW_GROUP);
    *out=*l;
    return 0;
}

static void launch_snapshot(struct security_session *s, unsigned which, struct ciuki_security_launch *out)
{
    *out=s->launches[which];
    struct process *p=proc_find(out->pid);
    int32_t *fds[]={&out->child_ready_fd,&out->child_console_fd,&out->child_display_fd,
        &out->child_input_fd,&out->child_service_fd};
    for (unsigned i=0;i<ARRAY_SIZE(fds);i++) if (*fds[i]>=0 &&
        (!p || security_process_generation(p)!=out->process_generation || !p->fds ||
         p->fds[*fds[i]].object!=s->child_objects[which][i])) *fds[i]=-1;
    struct process *controller=proc_find(security_subject_pid(s->controllers[which]));
    if (out->control_fd>=0 && (!controller || controller->subject!=s->controllers[which] ||
        !controller->fds || controller->fds[out->control_fd].object!=s->control_objects[which])) out->control_fd=-1;
}

static bool record_zero(const void *record, uint32_t bytes)
{
    const uint8_t *p=record;
    for (uint32_t i=0;i<bytes;i++) if (p[i]) return false;
    return true;
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
    if (a.operation < 1 || a.operation > 14) return -EINVAL;
    bool extended = a.operation >= 8;
    bool query = a.operation <= CIUKI_SECURITY_QUERY_PEER;
    bool create = a.operation == CIUKI_SECURITY_SESSION_CREATE;
    bool spawn = a.operation == CIUKI_SECURITY_SPAWN_CREDENTIAL;
    bool end = a.operation == CIUKI_SECURITY_SESSION_END;
    if ((create || spawn || end || (a.operation>=8 && a.operation<=11)) && !security_logon(p)) return -EPERM;
    if (!security_process_live(p)) return -EACCES;
    if ((a.flags && !end) || (end && a.flags != CIUKI_SECURITY_END_BEGIN && a.flags != CIUKI_SECURITY_END_COLLECT) ||
        (!create && (a.credential_address || a.profile_address)) || (!spawn && !extended && a.spawn_address) ||
        (a.operation != CIUKI_SECURITY_QUERY_PROCESS && a.target_pid) ||
        (a.operation != CIUKI_SECURITY_QUERY_PEER && a.endpoint_fd) ||
        ((query || create) && a.session_id) || (!query && !extended && (a.output_address || a.output_bytes))) return -EINVAL;
    if (query && a.output_bytes != 64) return -EINVAL;
    if (a.operation == CIUKI_SECURITY_QUERY_PROCESS && (!a.target_pid || a.target_pid > INT32_MAX)) return -EINVAL;
    if (a.operation == CIUKI_SECURITY_QUERY_PEER && (a.endpoint_fd < 0 || a.endpoint_fd >= CIUKI_OPEN_MAX)) return -EBADF;
    if (end && !a.session_id) return -EINVAL;
    union {
        struct ciuki_security_launch launch;
        struct ciuki_security_console console;
        struct ciuki_security_link link;
    } input={0}, output={0};
    uint32_t record_bytes=0;
    bool extended_output=extended && a.operation!=CIUKI_SECURITY_SERVICE_CANCEL;
    if (extended) {
        bool launch=a.operation<=9, console=a.operation==10 || a.operation==11;
        record_bytes=launch ? 96 : 64;
        if (a.flags || a.target_pid || a.endpoint_fd || a.credential_address || a.profile_address ||
            (a.operation>=12 && a.session_id) ||
            (extended_output ? a.output_bytes!=record_bytes : (a.output_bytes || a.output_address))) return -EINVAL;
        if ((err=security_span(a.spawn_address,record_bytes))) return err;
        if (security_overlap(tf->ebx,64,a.spawn_address,record_bytes)) return -EINVAL;
        if ((err=copy_from_user(&input,a.spawn_address,record_bytes))) return err;
        if (input.link.version!=1) return -EOPNOTSUPP;
        if (input.link.size!=record_bytes) return -EINVAL;
        if (extended_output) {
            if ((err=security_span(a.output_address,record_bytes))) return err;
            if (security_overlap(a.output_address,record_bytes,tf->ebx,64) ||
                security_overlap(a.output_address,record_bytes,a.spawn_address,record_bytes)) return -EINVAL;
        }
        /* Compare against a selector-only record. This rejects every unused
         * field, including signed fds, padding, copied outputs and reserved. */
        output=input;
        if (launch) {
            if (input.launch.session_id!=a.session_id || input.launch.purpose<1 || input.launch.purpose>2) return -EINVAL;
            output.launch.purpose=output.launch.session_id=output.launch.session_generation=0;
            if (a.operation==9) output.launch.launch_id=output.launch.launch_generation=0;
        } else if (console) {
            if (input.console.session_id!=a.session_id || input.console.reason<1 || input.console.reason>4) return -EINVAL;
            if (a.operation==10) {
                output.console.session_id=output.console.session_generation=output.console.reason=0;
            } else {
                /* LEAVE uses the whole current tuple, except state/reserved. */
                if (input.console.state || !record_zero(input.console.reserved,12)) return -EINVAL;
                memset(&output,0,sizeof(output)); output.link.version=1; output.link.size=64;
            }
        } else {
            if (input.link.service<1 || input.link.service>6) return -EINVAL;
            output.link.service=0;
            if (a.operation==14) {
                if (!(input.link.request_id_lo|input.link.request_id_hi) || !input.link.endpoint_generation) return -EINVAL;
                output.link.endpoint_fd=0; output.link.endpoint_generation=0;
                output.link.request_id_lo=output.link.request_id_hi=0;
            }
        }
        output.link.version=output.link.size=0;
        if (!record_zero(&output,record_bytes)) return -EINVAL;
        memset(&output,0,sizeof(output));
    }
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
    if ((query || extended_output) && (err = ua_pin(p->memory, &out_pin, a.output_address,
        query ? 64 : record_bytes, true))) goto done;
    if (!security_process_live(p)) { err = -EACCES; goto done; }
    switch (a.operation) {
    case CIUKI_SECURITY_QUERY_SELF: credential = *security_credential(p); break;
    case CIUKI_SECURITY_QUERY_PROCESS: err = security_query_process(p, a.target_pid, &credential); break;
    case CIUKI_SECURITY_QUERY_PEER: {
        struct security_peer peer = {0};
        err = channel_peer_get ? channel_peer_get(p, a.endpoint_fd, &peer) : -EBADF;
        if (!err) {
            if (!channel_peer_live(&peer)) err = -ESRCH;
            else { credential = *security_subject_credential(peer.subject); a.result=peer.generation; }
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
    case CIUKI_SECURITY_SESSION_QUERY:
        err = security_session_query(p, a.session_id, &a.state);
        if (!err) a.result=security_session_generation(a.session_id);
        break;
    case CIUKI_SECURITY_SESSION_LAUNCH: case CIUKI_SECURITY_LAUNCH_POLL:
        err=session_launch(p,&input.launch,&output.launch,a.operation==9); break;
    case CIUKI_SECURITY_SECURE_ENTER:
        err=console_enter(p,a.session_id,&input.console,&output.console,false); break;
    case CIUKI_SECURITY_SECURE_LEAVE:
        err=console_leave(p,a.session_id,&input.console,&output.console); break;
    case CIUKI_SECURITY_SERVICE_CONNECT: case CIUKI_SECURITY_SERVICE_ACCEPT: case CIUKI_SECURITY_SERVICE_CANCEL:
        err=channel_security_link ? channel_security_link(p,a.operation,&input.link,&output.link,&a.result) : -EIO;
        break;
    }
    if (!err) {
        if (query) err = copy_to_user(a.output_address, &credential, 64);
        if (!err && extended_output) err=copy_to_user(a.output_address,&output,record_bytes);
        if (!err) err = copy_to_user(tf->ebx, &a, 64);
    }
done:
    if (query || extended_output) ua_unpin(p->memory, &out_pin);
    ua_unpin(p->memory, &args_pin);
    return err;
}
