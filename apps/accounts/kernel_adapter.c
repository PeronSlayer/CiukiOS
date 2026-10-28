/* SPDX-License-Identifier: MIT
 * Production acct_kernel binding over call 101 (sec-k01). See kernel_adapter.h.
 *
 * Identity rules. The kernel assigns one endpoint generation per connected
 * pair from a boot-wide monotonic counter that is never reused (exhaustion
 * refuses new pairs), and QUERY_PEER(fd) reports that generation with the
 * peer credential only while the link is live: revocation, cancellation,
 * timeout or peer death make it ESRCH (channel_peer_get/channel_peer_live,
 * the same primitives behind security_subject_acquire_endpoint). A retained
 * subject is a lease on an accepted endpoint descriptor that this process
 * keeps open until the last lease and exchange are gone, so the number cannot
 * be reused beneath it. Two retained subjects are the same link exactly when
 * both leases are live and name the same endpoint generation, peer process
 * incarnation and service. Lease handles and copied subject metadata are
 * never compared as identity. Kernel-resident brokers use the internal
 * security_subject_same_link instead; no new call-101 operation is needed.
 */
#include "kernel_adapter.h"
#include "accounts.h"
#include "kernel_sys.h"
#include "wire.h"
#include <ciuki/abi.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CSC1 0x31435343u
#define OUTCOME_TIMEOUT_MS CIUKI_SECURITY_SERVICE_TIMEOUT_MS

static struct acct_kernel_adapter *adapter_of(void *ctx) { return ctx; }
static int security(uint32_t op, struct ciuki_security_args *a, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes)
{
    a->version=1; a->size=64; a->operation=op;
    return acct_sys_security(a,in,in_bytes,out,out_bytes);
}
static int query_peer(int fd, struct ciuki_credential *c, uint32_t *generation)
{
    struct ciuki_security_args a={ .endpoint_fd=fd };
    int e=security(CIUKI_SECURITY_QUERY_PEER,&a,NULL,0,c,64);
    if (!e) *generation=a.result;
    return e;
}
/* 1 while QUERY_PEER still reports this exact link generation. */
static int link_live(const struct acct_kernel_link *l)
{
    struct ciuki_credential c; uint32_t generation=0;
    if (!l->occupied || l->fd<0) return 0;
    if (query_peer(l->fd,&c,&generation)) return 0;
    return generation==l->endpoint_generation;
}
static struct acct_kernel_link *link_for_fd(struct acct_kernel_adapter *k, int fd)
{
    for (unsigned i=0;i<ACCT_KERNEL_LINKS;i++)
        if (k->links[i].occupied && k->links[i].fd==fd) return &k->links[i];
    return NULL;
}
static struct acct_kernel_lease *lease_for(struct acct_kernel_adapter *k, uintptr_t retained)
{
    unsigned index=(unsigned)(retained&255u);
    uint32_t serial=(uint32_t)((retained>>8)&0xffffffu);
    if (!index || index>ACCT_KERNEL_LEASES || (uint64_t)retained>>32) return NULL;
    struct acct_kernel_lease *l=&k->leases[index-1];
    if (!l->occupied || l->serial!=serial || !k->links[l->link].occupied) return NULL;
    return l;
}
static struct acct_kernel_link *link_of(struct acct_kernel_adapter *k, const struct acct_subject *s)
{
    struct acct_kernel_lease *l=s ? lease_for(k,s->retained) : NULL;
    return l ? &k->links[l->link] : NULL;
}
static int link_referenced(const struct acct_kernel_adapter *k, unsigned index)
{
    if (k->links[index].active_request) return 1;
    for (unsigned i=0;i<ACCT_KERNEL_LEASES;i++)
        if (k->leases[i].occupied && k->leases[i].link==index) return 1;
    return 0;
}
/* A retiring endpoint closes once nothing refers to it any more. */
static void link_release(struct acct_kernel_adapter *k, unsigned index)
{
    struct acct_kernel_link *l=&k->links[index];
    if (!l->occupied || !l->retiring || link_referenced(k,index)) return;
    acct_sys_close(l->fd);
    memset(l,0,sizeof(*l)); l->fd=-1;
}
static void record_link(struct acct_kernel_link *l, const struct ciuki_security_link *r)
{
    *l=(struct acct_kernel_link){ .fd=r->endpoint_fd, .endpoint_generation=r->endpoint_generation,
        .service=r->service, .service_generation=r->service_generation, .peer_pid=r->peer_pid,
        .peer_process_generation=r->peer_process_generation, .peer_role=r->peer_role,
        .session_id=r->session_id, .occupied=1 };
}
static void drop_logon(struct acct_kernel_adapter *k)
{
    if (k->logon.occupied && k->logon.fd>=0) acct_sys_close(k->logon.fd);
    memset(&k->logon,0,sizeof(k->logon)); k->logon.fd=-1;
}
/* Private ACCOUNT->LOGON link. ESRCH while no LOGON role is registered. */
static int logon_connect(struct acct_kernel_adapter *k)
{
    if (k->logon.occupied && link_live(&k->logon)) return 0;
    drop_logon(k);
    struct ciuki_security_link in={ .version=1, .size=64, .service=CIUKI_SECURITY_SERVICE_LOGON }, out;
    struct ciuki_security_args a={0};
    int e=security(CIUKI_SECURITY_SERVICE_CONNECT,&a,&in,64,&out,64);
    if (e) return e;
    if (out.server_role!=CIUKI_SECURITY_ROLE_LOGON || out.peer_role!=CIUKI_SECURITY_ROLE_LOGON) {
        acct_sys_close(out.endpoint_fd); return -EPERM;
    }
    record_link(&k->logon,&out); k->logon_request=0;
    return 0;
}
/* The registered role instance behind an accepted peer. The kernel reports
 * a role generation only for the server of a link, so a LOGON peer's
 * generation is read from the ACCOUNT->LOGON link to the same process
 * incarnation; other roles carry none here. */
static uint32_t peer_role_generation(struct acct_kernel_adapter *k, const struct acct_kernel_link *l)
{
    if (l->peer_role==CIUKI_SECURITY_ROLE_ACCOUNT && l->peer_pid==k->pid) return k->generation;
    if (l->peer_role!=CIUKI_SECURITY_ROLE_LOGON) return 0;
    if (!k->logon.occupied || k->logon.peer_pid!=l->peer_pid ||
        k->logon.peer_process_generation!=l->peer_process_generation) {
        if (logon_connect(k)) return 0;
    }
    return k->logon.peer_pid==l->peer_pid && k->logon.peer_process_generation==l->peer_process_generation ?
        k->logon.service_generation : 0;
}
static uint32_t logon_generation(struct acct_kernel_adapter *k)
{ return logon_connect(k) ? 0 : k->logon.service_generation; }

/* Sessions as the kernel attests them to SYSTEM: SESSION_QUERY answers for
 * every issued id (DEAD for freed records) and ESRCH above the high water. */
static int session_state(uint32_t session, uint32_t *state)
{
    struct ciuki_security_args a={ .session_id=session };
    int e=security(CIUKI_SECURITY_SESSION_QUERY,&a,NULL,0,NULL,0);
    if (!e) *state=a.state;
    return e;
}
/* Owner of a session: the uid its live processes run as (QUERY_PROCESS).
 * -ENOENT without a live process, -EIO if they disagree (fail closed). */
static int session_owner(uint32_t session, uint32_t *uid)
{
    static uint32_t pids[CIUKI_PROCESS_MAX];
    int n=acct_sys_pids(pids,CIUKI_PROCESS_MAX), found=0;
    if (n<0) return n;
    for (int i=0;i<n;i++) {
        struct ciuki_credential c; struct ciuki_security_args a={ .target_pid=pids[i] };
        if (security(CIUKI_SECURITY_QUERY_PROCESS,&a,NULL,0,&c,64) || c.session_id!=session) continue; /* exited meanwhile */
        if (found && c.uid!=*uid) return -EIO;
        *uid=c.uid; found=1;
    }
    return found ? 0 : -ENOENT;
}
/* Visits the live (ACTIVE/CLOSING) sessions. uid==CIUKI_UID_INVALID asks
 * whether any exists; otherwise whether one belongs to uid. A live session
 * without a process (under construction or draining) cannot be attributed
 * and counts as a match: 1 match, 0 none, -errno. */
static int live_session(uint32_t uid)
{
    for (uint32_t id=1;id;id++) {
        uint32_t state=0, owner=0;
        int e=session_state(id,&state);
        if (e==-ESRCH) return 0;
        if (e) return e;
        if (state==CIUKI_SECURITY_SESSION_DEAD) continue;
        if (uid==CIUKI_UID_INVALID) return 1;
        e=session_owner(id,&owner);
        if (e==-ENOENT || (!e && owner==uid)) return 1;
        if (e) return e;
    }
    return -EAGAIN;
}

/* acct_kernel callbacks. */
static int lease_acquire(void *ctx)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    if (k->leased) return -EBUSY;
    struct ciuki_credential self; struct ciuki_security_args a={0};
    int e=security(CIUKI_SECURITY_QUERY_SELF,&a,NULL,0,&self,64);
    if (e) return e;
    if (self.uid!=CIUKI_UID_SYSTEM || self.session_id) return -EPERM;
    /* The kernel registers roles from its sealed manifest only. Connecting to
     * the ACCOUNTS service proves this process is that registration and
     * reveals its role generation; the unaccepted server end is reclaimed
     * when the client end closes. */
    struct ciuki_security_link in={ .version=1, .size=64, .service=CIUKI_SECURITY_SERVICE_ACCOUNTS }, out;
    memset(&a,0,sizeof(a));
    e=security(CIUKI_SECURITY_SERVICE_CONNECT,&a,&in,64,&out,64);
    if (e) return e==-ESRCH ? -EPERM : e;
    int own=out.server_role==CIUKI_SECURITY_ROLE_ACCOUNT && out.peer_pid==acct_sys_pid();
    acct_sys_close(out.endpoint_fd);
    if (!own) return -EPERM;
    /* Safe Mode is this start's kernel boot flag; without it the service
     * cannot attest the CiukiAdministrator exception, so it fails closed. */
    uint32_t flags=0;
    e=acct_sys_boot_flags(&flags);
    if (e) return e;
    k->pid=out.peer_pid; k->generation=out.service_generation;
    k->boot_flags=flags; k->safe_mode=!!(flags&CIUKI_BOOT_SAFE_MODE);
    k->leased=1;
    return 0;
}
static void lease_release(void *ctx)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    drop_logon(k); k->leased=0;
}
static int retain_peer(void *ctx, int fd, struct acct_subject *s)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    struct acct_kernel_link *l=link_for_fd(k,fd);
    if (!l) return -EBADF;
    if (l->retiring) return -ESRCH;
    struct ciuki_credential c; uint32_t generation=0;
    int e=query_peer(fd,&c,&generation);
    if (e) return e;
    if (generation!=l->endpoint_generation) return -ESRCH;
    if (c.group_count>8) return -EACCES;
    struct acct_kernel_lease *lease=NULL;
    for (unsigned i=0;i<ACCT_KERNEL_LEASES && !lease;i++) if (!k->leases[i].occupied) lease=&k->leases[i];
    if (!lease) return -EAGAIN;
    k->serial=(k->serial+1)&0xffffffu; if (!k->serial) k->serial=1;
    *lease=(struct acct_kernel_lease){ .link=(unsigned)(l-k->links), .serial=k->serial, .refs=1, .occupied=1 };
    memset(s,0,sizeof(*s));
    s->retained=((uintptr_t)lease->serial<<8)|(uintptr_t)((lease-k->leases)+1);
    s->uid=c.uid; s->group_count=c.group_count; memcpy(s->groups,c.groups,sizeof(s->groups));
    s->session_id=c.session_id; s->safe_mode=k->safe_mode;
    s->role=l->peer_role; s->service=l->service; s->endpoint_generation=l->endpoint_generation;
    s->service_generation=l->service_generation; s->role_generation=peer_role_generation(k,l);
    return 0;
}
static void subject_get(void *ctx, const struct acct_subject *s)
{
    struct acct_kernel_lease *l=lease_for(adapter_of(ctx),s->retained);
    if (l) l->refs++;
}
static void release_peer(void *ctx, struct acct_subject *s)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    struct acct_kernel_lease *l=lease_for(k,s->retained);
    if (l && !--l->refs) {
        unsigned link=l->link;
        memset(l,0,sizeof(*l));
        link_release(k,link);
    }
    acct_wipe(s,sizeof(*s));
}
static int subject_live(struct acct_kernel_adapter *k, const struct acct_subject *s)
{
    struct acct_kernel_link *l=link_of(k,s);
    if (!l || !link_live(l)) return 0;
    return !s->request_id || l->active_request==s->request_id;
}
static int peer_alive(void *ctx, const struct acct_subject *s)
{ return subject_live(adapter_of(ctx),s); }
/* Sealed same_link backend: ciuki_accounts_same_link forwards both retained
 * handles here. 1 same live link, 0 different links, -EACCES/-ESRCH. */
static int same_link_backend(void *context, uintptr_t a, uintptr_t b)
{
    struct acct_kernel_adapter *k=adapter_of(context);
    struct acct_kernel_lease *la=lease_for(k,a),*lb=lease_for(k,b);
    if (!la || !lb) return -EACCES;
    struct acct_kernel_link *x=&k->links[la->link],*y=&k->links[lb->link];
    if (!link_live(x) || !link_live(y)) return -ESRCH;
    return x->endpoint_generation==y->endpoint_generation && x->peer_pid==y->peer_pid &&
        x->peer_process_generation==y->peer_process_generation && x->service==y->service;
}
/* 1 when uid owns a live (ACTIVE/CLOSING) session, conservatively also while
 * a live session has no process to attribute it to; 0 otherwise. */
static int active_uid(void *ctx, uint32_t uid)
{ (void)ctx; return uid==CIUKI_UID_INVALID ? -EINVAL : live_session(uid); }
static uint64_t monotonic_ms(void *ctx) { (void)ctx; return acct_sys_monotonic_ms(); }
static int entropy(void *ctx, void *p, size_t n) { (void)ctx; return acct_sys_entropy(p,n); }
static void *allocate(void *ctx, size_t n)
{
    void *p=malloc(n);
    if (p) { memset(p,0,n); adapter_of(ctx)->allocations++; }
    return p;
}
static void deallocate(void *ctx, void *p, size_t n)
{ (void)n; if (p) { free(p); adapter_of(ctx)->allocations--; } }
/* Catalogue pictures live below the SYSTEM-maintained picture directory. */
static int picture_valid(void *ctx, uint32_t uid, const char *relative)
{
    struct acct_kernel_adapter *k=adapter_of(ctx); (void)uid;
    if (!relative || !memchr(relative,0,96)) return 0;
    if (!*relative) return 1; /* default picture */
    if (!acct_picture_syntax(relative)) return 0;
    char path[64+96+2]; size_t root=strlen(k->picture_root);
    if (root+1+strlen(relative)>=sizeof(path)) return 0;
    memcpy(path,k->picture_root,root); path[root]='/'; strcpy(path+root+1,relative);
    return acct_sys_file_present(path)==1;
}
/* The kernel exposes console/authentication epochs to the registered LOGON
 * role only (SECURE_ENTER/LEAVE): ACCOUNT trusts the retained live LOGON
 * role instance to relay them and the UI->LOGON generation (contract A2-1).
 * What the kernel attests to ACCOUNT is checked here: the LOGON peer's live
 * link, role, uid/session, both current role generations and, for a session
 * input, that the session is ACTIVE. The locked owner is the uid every live
 * process of that session runs as; CIUKI_UID_INVALID (UNLOCK refused) when
 * there is no session input or no attributable owner. */
static int auth_binding(void *ctx, const struct acct_subject *p, uint32_t ui_generation, uint32_t session,
                        uint32_t console, uint32_t auth_epoch, uint32_t lg, uint32_t ag, uint32_t *locked_uid)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    (void)ui_generation; (void)console;
    if (!locked_uid) return -EINVAL;
    *locked_uid=CIUKI_UID_INVALID;
    if (!subject_live(k,p) || p->role!=CIUKI_SECURITY_ROLE_LOGON || p->uid || p->session_id) return -EAGAIN;
    if (ag!=k->generation || lg!=logon_generation(k) || !lg || !auth_epoch) return -EAGAIN;
    if (session) {
        uint32_t state=0, owner=0;
        if (session_state(session,&state) || state!=CIUKI_SECURITY_SESSION_ACTIVE) return -EAGAIN;
        if (!session_owner(session,&owner)) *locked_uid=owner;
    }
    return 0;
}
static int gate_state(void *ctx, uint32_t session, uint32_t *console, uint32_t *auth, uint32_t *uid)
{
    (void)ctx; (void)session; (void)console; (void)auth; (void)uid;
    /* LOGON-only kernel state; the ACCOUNT process never owns a gate. */
    return -EPERM;
}
static int role_generations(void *ctx, uint32_t *logon, uint32_t *account)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    if (!logon || !account) return -EINVAL;
    *account=k->generation; *logon=logon_generation(k);
    return 0;
}
static int frame_send(int fd, uint32_t service, uint32_t op, uint64_t id, uint32_t generation,
                      const void *body, size_t total, size_t offset)
{
    struct ciuki_message m; size_t fragment=total-offset;
    if (fragment>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) fragment=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
    memset(&m,0,sizeof(m)); uint8_t *p=m.data;
    w32(p,CSC1); w32(p+4,1); w32(p+8,64); w32(p+12,service); w32(p+16,op); w32(p+20,1);
    w64(p+24,id); w32(p+32,generation); w32(p+36,(uint32_t)fragment); w32(p+52,(uint32_t)total); w32(p+56,(uint32_t)offset);
    if (fragment) memcpy(p+64,(const uint8_t *)body+offset,fragment);
    m.length=(uint32_t)(64+fragment);
    int e=acct_sys_channel_send(fd,&m);
    acct_wipe(&m,sizeof(m));
    return e;
}
/* ACCOUNT -> LOGON AUTH_OUTCOME=1 on the private link, acknowledged by the
 * reply status. The subject is the retained LOGON peer whose attempt it is;
 * the connected LOGON must be that same process incarnation. */
static int auth_outcome(void *ctx, const struct acct_subject *p, uint64_t attempt, int frame_status,
                        const void *body, size_t n)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    struct acct_auth_outcome decoded;
    int e=frame_status || !body || n!=304 || acct_outcome_decode(&decoded,0,body,n) || decoded.challenge.attempt!=attempt;
    acct_wipe(&decoded,sizeof(decoded));
    if (e) return -EINVAL;
    struct acct_kernel_link *l=link_of(k,p);
    if (!l || !link_live(l) || l->peer_role!=CIUKI_SECURITY_ROLE_LOGON) return -ESRCH;
    e=logon_connect(k);
    if (e) return e;
    if (k->logon.peer_pid!=l->peer_pid || k->logon.peer_process_generation!=l->peer_process_generation) return -EAGAIN;
    uint64_t id=++k->logon_request;
    for (size_t offset=0;;offset+=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) {
        e=frame_send(k->logon.fd,CIUKI_SECURITY_SERVICE_LOGON,1,id,k->logon.endpoint_generation,body,n,offset);
        if (e) { drop_logon(k); return e; }
        if (offset+CIUKI_SECURITY_SERVICE_FRAGMENT_MAX>=n) break;
    }
    uint64_t deadline=acct_sys_monotonic_ms()+OUTCOME_TIMEOUT_MS;
    for (;;) {
        uint64_t now=acct_sys_monotonic_ms();
        if (now>=deadline) { drop_logon(k); return -ETIMEDOUT; }
        int fd=k->logon.fd; int ready=acct_sys_wait_readable(&fd,1,(unsigned)(deadline-now));
        if (ready<0) { drop_logon(k); return ready; }
        if (!ready) continue;
        struct ciuki_message m; int r=acct_sys_channel_recv(k->logon.fd,&m,0);
        if (r==-EAGAIN) continue;
        if (r<=0) { drop_logon(k); return r ? r : -ESRCH; }
        const uint8_t *h=m.data;
        /* Only the CSC1 reply of this request can arrive on this link. */
        if (m.length<64 || a32(h)!=CSC1 || a32(h+20)!=2 || a64(h+24)!=id || a32(h+16)!=1) { drop_logon(k); return -EIO; }
        int32_t status=(int32_t)a32(h+48); uint32_t total=a32(h+52), offset=a32(h+56), bytes=a32(h+36);
        if (offset+bytes<total) continue; /* an acknowledgment body, if any, drains */
        return status;
    }
}
/* Enrollment runs only before any ordinary session exists (contract A2-1);
 * LOGON's confirmed protected workflow is its own precondition. */
static int enrollment_authorized(void *ctx, const struct acct_subject *p)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    if (!subject_live(k,p) || p->role!=CIUKI_SECURITY_ROLE_LOGON || p->uid || p->session_id) return -EACCES;
    int active=live_session(CIUKI_UID_INVALID);
    return active ? (active<0 ? active : -EACCES) : 0;
}
/* Trusted LOCK through SECURE_ENTER with the current session generation.
 * The kernel grants operations 8..11 to the registered LOGON role only, so
 * from the ACCOUNT process this is EPERM; LOGON links the same binding. */
static int session_lock(void *ctx, const struct acct_subject *p, uint32_t session)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    if (!session || !subject_live(k,p) || p->role!=CIUKI_SECURITY_ROLE_DESKTOP || p->session_id!=session) return -EACCES;
    struct ciuki_security_args a={ .session_id=session };
    int e=security(CIUKI_SECURITY_SESSION_QUERY,&a,NULL,0,NULL,0);
    if (e) return e;
    struct ciuki_security_console in={ .version=1, .size=64, .session_id=session, .session_generation=a.result,
        .reason=CIUKI_SECURITY_SECURE_LOCK }, out;
    memset(&a,0,sizeof(a)); a.session_id=session;
    return security(CIUKI_SECURITY_SECURE_ENTER,&a,&in,64,&out,64);
}
/* Operations 12..14 with the frozen 64-byte record, unchanged. CANCEL
 * returns its args.result (1 proven pre-admission removal, 0 otherwise). */
static int service_link(void *ctx, unsigned op, const struct acct_security_link *in, struct acct_security_link *out)
{
    (void)ctx;
    _Static_assert(sizeof(struct acct_security_link)==64 && sizeof(struct ciuki_security_link)==64, "link record");
    if (!in || in->version!=1 || in->size!=64 || op<ACCT_SERVICE_CONNECT || op>ACCT_SERVICE_CANCEL) return -EINVAL;
    if (op!=ACCT_SERVICE_CANCEL && !out) return -EINVAL;
    struct ciuki_security_link record, result; memcpy(&record,in,64);
    struct ciuki_security_args a={0};
    int e=security(op,&a,&record,64,op==ACCT_SERVICE_CANCEL ? NULL : &result,op==ACCT_SERVICE_CANCEL ? 0 : 64);
    if (e) return e;
    if (op==ACCT_SERVICE_CANCEL) return (int)a.result;
    memcpy(out,&result,64);
    return 0;
}
static unsigned exchanges(const struct acct_kernel_adapter *k)
{
    unsigned n=0;
    for (unsigned i=0;i<ACCT_KERNEL_LINKS;i++) n+=k->links[i].occupied && k->links[i].active_request;
    return n;
}
static int exchange_begin(void *ctx, const struct acct_subject *p, uint64_t id)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    struct acct_kernel_link *l=link_of(k,p);
    if (!l || l->retiring || !link_live(l)) return -ESRCH;
    if (l->active_request) return -EAGAIN;
    if (exchanges(k)>=CIUKI_SECURITY_SERVICE_PENDING_MAX) return -EAGAIN;
    if (!id || id<=l->last_request) return -EINVAL;
    l->active_request=l->last_request=id;
    return 0;
}
/* Reply frames for the original link/request only; the kernel also checks
 * the pair generation in every frame, so a reused descriptor cannot
 * receive them. */
static int deliver_reply(void *ctx, const struct acct_subject *p, unsigned op, uint64_t generation, int status,
                         const void *body, size_t n)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    struct acct_kernel_link *l=link_of(k,p);
    if (!l || !link_live(l) || !p->request_id || l->active_request!=p->request_id) return -ESRCH;
    if ((status && n) || n>CIUKI_SECURITY_SERVICE_BODY_MAX) return -EINVAL;
    uint8_t frame[256]; struct ciuki_message m; int e=0;
    for (size_t offset=0;!e;offset+=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) {
        size_t bytes=0;
        e=acct_reply_frame(p,op,p->request_id,generation,status,body,n,offset,frame,&bytes);
        if (e) break;
        memset(&m,0,sizeof(m)); m.length=(uint32_t)bytes; memcpy(m.data,frame,bytes);
        e=acct_sys_channel_send(l->fd,&m);
        if (offset+CIUKI_SECURITY_SERVICE_FRAGMENT_MAX>=n) break;
    }
    acct_wipe(frame,sizeof(frame)); acct_wipe(&m,sizeof(m));
    return e;
}
static void exchange_end(void *ctx, const struct acct_subject *p)
{
    struct acct_kernel_adapter *k=adapter_of(ctx);
    struct acct_kernel_link *l=link_of(k,p);
    if (l && p->request_id && l->active_request==p->request_id) {
        l->active_request=0;
        link_release(k,(unsigned)(l-k->links));
    }
}

int acct_kernel_adapter_init(struct acct_kernel_adapter *k, struct acct_kernel *out)
{
    if (!k || !out) return -EINVAL;
    memset(k,0,sizeof(*k));
    k->binding.context=k; k->binding.same_link=same_link_backend;
    k->logon.fd=-1;
    for (unsigned i=0;i<ACCT_KERNEL_LINKS;i++) k->links[i].fd=-1;
    memcpy(k->picture_root,ACCT_KERNEL_PICTURES,sizeof(ACCT_KERNEL_PICTURES));
    *out=(struct acct_kernel){ .ctx=&k->binding, .real=1,
        .lease_acquire=lease_acquire, .lease_release=lease_release, .retain_peer=retain_peer,
        .subject_get=subject_get, .peer_alive=peer_alive, .same_link=ciuki_accounts_same_link,
        .release_peer=release_peer, .active_uid=active_uid, .monotonic_ms=monotonic_ms, .entropy=entropy,
        .allocate=allocate, .deallocate=deallocate, .picture_valid=picture_valid, .auth_binding=auth_binding,
        .gate_state=gate_state, .role_generations=role_generations, .auth_outcome=auth_outcome,
        .enrollment_authorized=enrollment_authorized, .session_lock=session_lock, .service_link=service_link,
        .exchange_begin=exchange_begin, .deliver_reply=deliver_reply, .exchange_end=exchange_end };
    return 0;
}
int acct_kernel_adapter_accept(struct acct_kernel_adapter *k, uint32_t service, int *fd)
{
    if (!k || !fd) return -EINVAL; /* ACCEPT is kernel-authorized by role, lease or not */
    struct acct_kernel_link *slot=NULL;
    for (unsigned i=0;i<ACCT_KERNEL_LINKS && !slot;i++) if (!k->links[i].occupied) slot=&k->links[i];
    if (!slot) return -ENOSPC;
    struct ciuki_security_link in={ .version=1, .size=64, .service=service }, out;
    struct ciuki_security_args a={0};
    int e=security(CIUKI_SECURITY_SERVICE_ACCEPT,&a,&in,64,&out,64);
    if (e) return e;
    if (out.server_role!=CIUKI_SECURITY_ROLE_ACCOUNT || out.service!=service) { acct_sys_close(out.endpoint_fd); return -EPERM; }
    if (link_for_fd(k,out.endpoint_fd)) { acct_sys_close(out.endpoint_fd); return -EEXIST; }
    record_link(slot,&out); *fd=out.endpoint_fd;
    return 0;
}
int acct_kernel_adapter_link_live(struct acct_kernel_adapter *k, int fd)
{
    struct acct_kernel_link *l=k ? link_for_fd(k,fd) : NULL;
    return l ? link_live(l) : -EBADF;
}
int acct_kernel_adapter_retire(struct acct_kernel_adapter *k, int fd)
{
    struct acct_kernel_link *l=k ? link_for_fd(k,fd) : NULL;
    if (!l) return -EBADF;
    unsigned index=(unsigned)(l-k->links);
    l->retiring=1;
    link_release(k,index);
    return k->links[index].occupied ? -EBUSY : 0;
}
unsigned acct_kernel_adapter_fds(const struct acct_kernel_adapter *k, int *fds, unsigned capacity)
{
    unsigned n=0;
    for (unsigned i=0;k && i<ACCT_KERNEL_LINKS && n<capacity;i++)
        if (k->links[i].occupied && !k->links[i].retiring) fds[n++]=k->links[i].fd;
    return n;
}
void acct_kernel_adapter_shutdown(struct acct_kernel_adapter *k)
{ if (k) drop_logon(k); }
