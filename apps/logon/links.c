/* SPDX-License-Identifier: MIT
 * LOGON endpoints (logon.h). Accepted service-5 links from LOGON_UI,
 * ACCOUNT and the session DESKTOP; LOGON's own client link to ACCOUNT's AUTH
 * endpoint; CSC1 framing; and the acct_kernel binding of the frozen
 * correlation core (apps/accounts/logon.c).
 *
 * Identity rules match the account service's binding (kernel_adapter.c): a
 * retained subject is a lease on a link this process keeps open; liveness is
 * QUERY_PEER generation equality on that descriptor; two leases are the same
 * link only when they name the same slot and serial while it is live. Peer
 * role, process and session come from the kernel's link record (ACCEPT or
 * CONNECT output) and QUERY_PEER, never from request bytes.
 */
#include "logon.h"
#include "../accounts/wire.h"
#include <errno.h>
#include <string.h>

#define CSC1 0x31435343u
#define AUTH_INDEX 255u

int lgn_security(struct lgn *g, uint32_t op, uint32_t session, uint32_t flags, const void *in, uint32_t in_bytes,
                 void *out, uint32_t out_bytes, struct ciuki_security_args *result)
{
    struct ciuki_security_args a;
    memset(&a,0,sizeof(a));
    a.version=1; a.size=64; a.operation=op; a.session_id=session; a.flags=flags;
    int e=g->sys->security(g->sys->ctx,&a,in,in_bytes,out,out_bytes);
    if (result) *result=a;
    return e;
}
static int query_peer(struct lgn *g, int fd, struct ciuki_credential *c, uint32_t *generation)
{
    struct ciuki_security_args a;
    memset(&a,0,sizeof(a));
    a.version=1; a.size=64; a.operation=CIUKI_SECURITY_QUERY_PEER; a.endpoint_fd=fd;
    int e=g->sys->security(g->sys->ctx,&a,NULL,0,c,64);
    if (!e) *generation=a.result;
    return e;
}
int lgn_link_live(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct ciuki_credential c; uint32_t generation=0;
    if (i>=LGN_LINKS || !l->occupied || l->fd<0) return 0;
    return !query_peer(g,l->fd,&c,&generation) && generation==l->endpoint_generation;
}
static int auth_live(struct lgn *g)
{
    struct ciuki_credential c; uint32_t generation=0;
    if (g->auth.fd<0) return 0;
    return !query_peer(g,g->auth.fd,&c,&generation) && generation==g->auth.endpoint_generation;
}
static void link_free(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    if (!l->occupied || l->refs) return;
    if (l->fd>=0) g->sys->close(g->sys->ctx,l->fd);
    lgn_wipe(l,sizeof(*l)); l->fd=-1;
}
/* A dead or refused link leaves the wait set at once; its slot (and its
 * number) stays until the last retained subject is released. */
static void link_retire(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    if (!l->occupied || l->retiring) return;
    l->retiring=1; l->pending=l->receiving=0;
    lgn_wipe(l->body,sizeof(l->body));
    uint32_t role=l->peer_role;
    lgn_link_lost(g,i,role);
    link_free(g,i);
}
void lgn_link_release(struct lgn *g, unsigned i)
{
    if (i>=LGN_LINKS) return;
    struct lgn_link *l=&g->links[i];
    if (l->refs) l->refs--;
    if (l->retiring) link_free(g,i);
}

/* ---- subjects for apps/accounts/logon.c ------------------------------- */
static struct lgn *self_of(void *ctx) { return ctx; }
static int resolve(struct lgn *g, uintptr_t retained, unsigned *index)
{
    unsigned i=(unsigned)(retained&255u); uint32_t serial=(uint32_t)((retained>>8)&0xffffffu);
    if (!serial || (uint64_t)retained>>32) return -EACCES;
    if (i==AUTH_INDEX) {
        if (g->auth.fd<0 || g->auth.serial!=serial) return -EACCES;
    } else if (!i || i>LGN_LINKS || !g->links[i-1].occupied || g->links[i-1].serial!=serial) return -EACCES;
    *index=i;
    return 0;
}
static int subject_live(struct lgn *g, const struct acct_subject *s)
{
    unsigned i;
    if (!s || resolve(g,s->retained,&i)) return 0;
    if (i==AUTH_INDEX) return auth_live(g);
    return !g->links[i-1].retiring && lgn_link_live(g,i-1);
}
static int k_peer_alive(void *ctx, const struct acct_subject *s) { return subject_live(self_of(ctx),s); }
static void k_subject_get(void *ctx, const struct acct_subject *s)
{
    struct lgn *g=self_of(ctx); unsigned i;
    if (!s || resolve(g,s->retained,&i)) return;
    if (i==AUTH_INDEX) g->auth.refs++; else g->links[i-1].refs++;
}
static void k_release_peer(void *ctx, struct acct_subject *s)
{
    struct lgn *g=self_of(ctx); unsigned i;
    if (s && !resolve(g,s->retained,&i)) {
        if (i==AUTH_INDEX) { if (g->auth.refs) g->auth.refs--; }
        else lgn_link_release(g,i-1);
    }
    if (s) lgn_wipe(s,sizeof(*s));
}
static int k_same_link(void *ctx, const struct acct_subject *a, const struct acct_subject *b)
{
    struct lgn *g=self_of(ctx); unsigned x,y;
    if (!a || !b || resolve(g,a->retained,&x) || resolve(g,b->retained,&y)) return -EACCES;
    if (!subject_live(g,a) || !subject_live(g,b)) return -ESRCH;
    return x==y;
}
static int k_role_generations(void *ctx, uint32_t *logon, uint32_t *account)
{
    struct lgn *g=self_of(ctx);
    if (!logon || !account) return -EINVAL;
    if (!g->own_generation || g->auth.fd<0 || !g->auth.service_generation) return -EAGAIN;
    *logon=g->own_generation; *account=g->auth.service_generation;
    return 0;
}
/* LOGON's own cached console bindings, refreshed by every SECURE_ENTER/LEAVE
 * it makes (coordinator.c). The kernel exposes them to this role only. */
static int k_gate_state(void *ctx, uint32_t session, uint32_t *console, uint32_t *epoch, uint32_t *uid)
{
    struct lgn *g=self_of(ctx);
    if (!console || !epoch || !uid) return -EINVAL;
    if (!session) {
        /* sec-user-04: an armed administrator log-off authenticates (action 2,
         * session 0) against the locked session's current console, so a
         * console change (attention, unlock, restart) invalidates it. */
        if (g->override && g->session.phase==LGN_ACTIVE && g->mode==LGN_MODE_LOCKED && g->session.console_valid &&
            g->session.console.state==CIUKI_SECURITY_CONSOLE_TRUSTED) {
            *console=g->session.console.console_generation; *epoch=g->session.console.authentication_epoch;
            *uid=CIUKI_UID_INVALID;
            return 0;
        }
        if (!g->boot_valid || g->session.phase) return -EAGAIN;
        *console=g->boot.console_generation; *epoch=g->boot.authentication_epoch; *uid=CIUKI_UID_INVALID;
        return 0;
    }
    if (session!=g->session.id || !g->session.console_valid) return -EAGAIN;
    *console=g->session.console.console_generation; *epoch=g->session.console.authentication_epoch;
    *uid=g->session.uid;
    return 0;
}
static uint64_t k_monotonic(void *ctx) { struct lgn *g=self_of(ctx); return g->sys->now_ms(g->sys->ctx); }
static int k_refuse(void *ctx) { (void)ctx; return -EPERM; }
static void k_nothing(void *ctx) { (void)ctx; }

int lgn_links_bind(struct lgn *g)
{
    for (unsigned i=0;i<LGN_LINKS;i++) g->links[i].fd=-1;
    g->auth.fd=-1;
    struct acct_kernel *k=&g->correlation.kernel;
    memset(k,0,sizeof(*k));
    k->ctx=g; k->real=1;
    k->lease_acquire=k_refuse; k->lease_release=k_nothing;
    k->subject_get=k_subject_get; k->peer_alive=k_peer_alive; k->same_link=k_same_link;
    k->release_peer=k_release_peer; k->monotonic_ms=k_monotonic; k->role_generations=k_role_generations;
    k->gate_state=k_gate_state;
    return 0;
}
static uint32_t next_serial(struct lgn *g)
{
    g->serial=(g->serial+1)&0xffffffu;
    if (!g->serial) g->serial=1;
    return g->serial;
}
int lgn_retain(struct lgn *g, unsigned i, struct acct_subject *s)
{
    if (i>=LGN_LINKS || !s) return -EINVAL;
    struct lgn_link *l=&g->links[i];
    struct ciuki_credential c; uint32_t generation=0;
    if (!l->occupied || l->retiring) return -ESRCH;
    int e=query_peer(g,l->fd,&c,&generation);
    if (e) return e;
    if (generation!=l->endpoint_generation || c.group_count>8) return -ESRCH;
    memset(s,0,sizeof(*s));
    s->retained=((uintptr_t)l->serial<<8)|(uintptr_t)(i+1);
    s->uid=c.uid; s->group_count=c.group_count; memcpy(s->groups,c.groups,sizeof(s->groups));
    s->session_id=c.session_id; s->safe_mode=g->safe;
    s->role=l->peer_role; s->service=LGN_SERVICE_LOGON; s->endpoint_generation=l->endpoint_generation;
    s->service_generation=l->service_generation;
    /* The kernel reports role generations for the server of a link. An
     * ACCOUNT peer's generation is read from LOGON's own AUTH link to the
     * same process incarnation; other peers carry none here. */
    if (l->peer_role==LGN_ROLE_ACCOUNT && g->auth.fd>=0 && g->auth.peer_pid==l->peer_pid &&
        g->auth.peer_process_generation==l->peer_process_generation) s->role_generation=g->auth.service_generation;
    l->refs++;
    return 0;
}
int lgn_retain_auth(struct lgn *g, struct acct_subject *s)
{
    struct ciuki_credential c; uint32_t generation=0;
    if (!s || g->auth.fd<0) return -ESRCH;
    int e=query_peer(g,g->auth.fd,&c,&generation);
    if (e) return e;
    if (generation!=g->auth.endpoint_generation || c.group_count>8) return -ESRCH;
    memset(s,0,sizeof(*s));
    s->retained=((uintptr_t)g->auth.serial<<8)|AUTH_INDEX;
    s->uid=c.uid; s->group_count=c.group_count; memcpy(s->groups,c.groups,sizeof(s->groups));
    s->session_id=c.session_id; s->safe_mode=g->safe;
    s->role=LGN_ROLE_ACCOUNT; s->service=LGN_SERVICE_AUTH; s->endpoint_generation=g->auth.endpoint_generation;
    s->service_generation=g->auth.service_generation; s->role_generation=g->auth.service_generation;
    g->auth.refs++;
    return 0;
}

/* ---- accepted service-5 links ---------------------------------------- */
void lgn_links_accept(struct lgn *g)
{
    for (unsigned n=0;n<LGN_LINKS;n++) {
        struct lgn_link *slot=NULL;
        for (unsigned i=0;i<LGN_LINKS && !slot;i++) if (!g->links[i].occupied) slot=&g->links[i];
        if (!slot) return; /* pending links time out in the kernel; clients retry */
        struct ciuki_security_link in, out;
        memset(&in,0,sizeof(in)); memset(&out,0,sizeof(out));
        in.version=1; in.size=64; in.service=LGN_SERVICE_LOGON;
        int e=lgn_security(g,CIUKI_SECURITY_SERVICE_ACCEPT,0,0,&in,64,&out,64,NULL);
        if (e) { if (e!=-EAGAIN) lgn_note(g,"[logon] accept failed: %d",e); return; }
        if (out.server_role!=LGN_ROLE_LOGON || out.service!=LGN_SERVICE_LOGON || !out.service_generation ||
            (out.peer_role!=LGN_ROLE_UI && out.peer_role!=LGN_ROLE_ACCOUNT && out.peer_role!=LGN_ROLE_DESKTOP)) {
            g->sys->close(g->sys->ctx,out.endpoint_fd);
            continue;
        }
        /* The accepted pair names this registered LOGON instance. */
        if (g->own_generation!=out.service_generation) {
            if (g->own_generation) lgn_note(g,"[logon] role generation changed %u -> %u",g->own_generation,out.service_generation);
            g->own_generation=out.service_generation;
        }
        memset(slot,0,sizeof(*slot));
        slot->occupied=1; slot->fd=out.endpoint_fd; slot->serial=next_serial(g);
        slot->endpoint_generation=out.endpoint_generation; slot->peer_role=out.peer_role; slot->peer_pid=out.peer_pid;
        slot->peer_process_generation=out.peer_process_generation; slot->session_id=out.session_id;
        slot->service_generation=out.service_generation;
    }
}
static void frame_header(uint8_t *h, uint32_t service, uint32_t op, uint64_t id, uint32_t endpoint, uint32_t flags,
                         uint32_t bytes, uint64_t generation, int status, uint32_t total, uint32_t offset)
{
    memset(h,0,64);
    w32(h,CSC1); w32(h+4,1); w32(h+8,64); w32(h+12,service); w32(h+16,op); w32(h+20,flags); w64(h+24,id);
    w32(h+32,endpoint); w32(h+36,bytes); w64(h+40,generation); w32(h+48,(uint32_t)status); w32(h+52,total); w32(h+56,offset);
}
int lgn_reply(struct lgn *g, unsigned i, int status, const void *body, size_t n)
{
    struct lgn_link *l=&g->links[i];
    if (i>=LGN_LINKS || !l->occupied || !l->pending || (status && n) || status>0 || n>LGN_REPLY_MAX) return -EINVAL;
    struct ciuki_message m; int e=0;
    for (size_t at=0;;) {
        size_t take=n-at; if (take>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) take=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
        memset(&m,0,sizeof(m));
        frame_header(m.data,LGN_SERVICE_LOGON,l->operation,l->request_id,l->endpoint_generation,2,(uint32_t)take,0,
                     status,(uint32_t)n,(uint32_t)at);
        if (take) memcpy(m.data+64,(const uint8_t *)body+at,take);
        m.length=(uint32_t)(64+take);
        e=g->sys->send(g->sys->ctx,l->fd,&m);
        lgn_wipe(&m,sizeof(m));
        if (e) break;
        at+=take;
        if (at>=n) break;
    }
    l->pending=0; l->attempt=0;
    lgn_wipe(l->body,sizeof(l->body)); l->received=l->total=0;
    if (e) { lgn_note(g,"[logon] reply failed op=%u: %d",l->operation,e); link_retire(g,i); }
    return e;
}
static void receive_link(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct ciuki_message m;
    for (unsigned burst=0;burst<32 && l->occupied && !l->retiring;burst++) {
        int r=g->sys->recv(g->sys->ctx,l->fd,&m);
        if (r==-EAGAIN) return;
        if (r!=1) { link_retire(g,i); return; }
        const uint8_t *h=m.data;
        uint32_t bytes=a32(h+36), total=a32(h+52), offset=a32(h+56);
        uint64_t id=a64(h+24);
        /* The kernel validated the frame shape; this checks the exchange. */
        if (m.fd_count || m.length<64 || m.length!=64+bytes || a32(h)!=CSC1 || a32(h+12)!=LGN_SERVICE_LOGON ||
            a32(h+20)!=1 || a32(h+32)!=l->endpoint_generation || l->pending || offset>total || bytes>total-offset) {
            lgn_wipe(&m,sizeof(m)); link_retire(g,i); return;
        }
        if (!l->receiving) {
            if (offset || !id || id<=l->last_request) { lgn_wipe(&m,sizeof(m)); link_retire(g,i); return; }
            l->receiving=1; l->request_id=l->last_request=id; l->operation=a32(h+16); l->frame_generation=a64(h+40);
            l->total=total; l->received=0; memcpy(l->header,h,64);
        } else if (id!=l->request_id || offset!=l->received || total!=l->total || a32(h+16)!=l->operation) {
            lgn_wipe(&m,sizeof(m)); link_retire(g,i); return;
        }
        if (total<=LGN_BODY_MAX && bytes) memcpy(l->body+offset,h+64,bytes);
        l->received+=bytes;
        lgn_wipe(&m,sizeof(m));
        if (l->received<l->total) continue;
        l->receiving=0; l->pending=1; l->pending_since=g->sys->now_ms(g->sys->ctx);
        if (l->total>LGN_BODY_MAX) { (void)lgn_reply(g,i,-EINVAL,NULL,0); continue; }
        lgn_request(g,i);
    }
}
void lgn_links_receive(struct lgn *g)
{
    for (unsigned i=0;i<LGN_LINKS;i++) {
        struct lgn_link *l=&g->links[i];
        if (!l->occupied || l->retiring) continue;
        receive_link(g,i);
        /* A peer that died with nothing queued reads EAGAIN; liveness is checked here. */
        if (l->occupied && !l->retiring && !lgn_link_live(g,i)) link_retire(g,i);
    }
}
unsigned lgn_wait_fds(const struct lgn *g, int *fds, unsigned capacity)
{
    unsigned n=0;
    for (unsigned i=0;i<LGN_LINKS && n<capacity;i++)
        if (g->links[i].occupied && !g->links[i].retiring) fds[n++]=g->links[i].fd;
    if (g->auth.fd>=0 && n<capacity) fds[n++]=g->auth.fd;
    return n;
}

/* ---- LOGON -> ACCOUNT AUTH client ------------------------------------ */
int lgn_auth_connect(struct lgn *g)
{
    if (g->auth.fd>=0) return 0;
    struct ciuki_security_link in, out;
    memset(&in,0,sizeof(in)); memset(&out,0,sizeof(out));
    in.version=1; in.size=64; in.service=LGN_SERVICE_AUTH;
    int e=lgn_security(g,CIUKI_SECURITY_SERVICE_CONNECT,0,0,&in,64,&out,64,NULL);
    if (e) return e;
    if (out.endpoint_fd<0 || out.server_role!=LGN_ROLE_ACCOUNT || out.peer_role!=LGN_ROLE_ACCOUNT ||
        !out.endpoint_generation || !out.service_generation) {
        if (out.endpoint_fd>=0) g->sys->close(g->sys->ctx,out.endpoint_fd);
        return -EPERM;
    }
    struct lgn_auth *a=&g->auth;
    unsigned refs=a->refs; /* leases on a previous link stay unresolvable: new serial */
    struct lgn_auth_request queue[LGN_QUEUE]; unsigned queued=a->queued;
    memcpy(queue,a->queue,sizeof(queue));
    memset(a,0,sizeof(*a));
    memcpy(a->queue,queue,sizeof(queue)); a->queued=queued; lgn_wipe(queue,sizeof(queue));
    a->refs=refs; a->fd=out.endpoint_fd; a->serial=next_serial(g); a->endpoint_generation=out.endpoint_generation;
    a->service_generation=out.service_generation; a->peer_pid=out.peer_pid; a->peer_process_generation=out.peer_process_generation;
    return 0;
}
/* The AUTH link died or must be abandoned: every queued and active request
 * fails (their UI requests get a status reply); leases resolve no more. */
void lgn_auth_drop(struct lgn *g)
{
    struct lgn_auth *a=&g->auth;
    struct lgn_auth_request active=a->active, queue[LGN_QUEUE];
    int busy=a->busy; unsigned queued=a->queued;
    memcpy(queue,a->queue,sizeof(queue));
    if (a->fd>=0) g->sys->close(g->sys->ctx,a->fd);
    unsigned refs=a->refs;
    lgn_wipe(a,sizeof(*a)); a->fd=-1; a->refs=refs;
    if (busy) lgn_auth_reply(g,&active,-ESRCH,NULL,0);
    for (unsigned i=0;i<queued;i++) lgn_auth_reply(g,&queue[i],-ESRCH,NULL,0);
    lgn_wipe(&active,sizeof(active)); lgn_wipe(queue,sizeof(queue));
    lgn_auth_lost(g);
}
int lgn_auth_queue(struct lgn *g, uint32_t op, uint64_t generation, const void *body, size_t n,
                   uint32_t purpose, uint32_t link, uint64_t attempt)
{
    struct lgn_auth *a=&g->auth;
    if (n>LGN_BODY_MAX || (n && !body)) return -EINVAL;
    if (a->queued>=LGN_QUEUE) return -EAGAIN;
    struct lgn_auth_request *q=&a->queue[a->queued++];
    memset(q,0,sizeof(*q));
    q->operation=op; q->generation=generation; q->bytes=(uint32_t)n; q->purpose=purpose; q->link=link; q->attempt=attempt;
    if (n) memcpy(q->body,body,n);
    lgn_auth_pump(g);
    return 0;
}
static int auth_send(struct lgn *g)
{
    struct lgn_auth *a=&g->auth; struct lgn_auth_request *q=&a->active;
    if (a->next_id==UINT64_MAX) return -EOVERFLOW;
    a->id=++a->next_id; a->got=a->total=0; a->status=0;
    a->sent_at=g->sys->now_ms(g->sys->ctx);
    struct ciuki_message m; int e=0;
    for (uint32_t at=0;;) {
        uint32_t take=q->bytes-at; if (take>CIUKI_SECURITY_SERVICE_FRAGMENT_MAX) take=CIUKI_SECURITY_SERVICE_FRAGMENT_MAX;
        memset(&m,0,sizeof(m));
        frame_header(m.data,LGN_SERVICE_AUTH,q->operation,a->id,a->endpoint_generation,1,take,q->generation,0,q->bytes,at);
        if (take) memcpy(m.data+64,q->body+at,take);
        m.length=64+take;
        e=g->sys->send(g->sys->ctx,a->fd,&m);
        lgn_wipe(&m,sizeof(m));
        if (e) break;
        at+=take;
        if (at>=q->bytes) break;
    }
    /* Relayed secrets live only until sent. */
    lgn_wipe(q->body,sizeof(q->body));
    return e;
}
void lgn_auth_pump(struct lgn *g)
{
    struct lgn_auth *a=&g->auth;
    if (a->busy || !a->queued) return;
    if (a->fd<0) {
        int e=lgn_auth_connect(g);
        if (e) {
            /* Not registered yet (ESRCH) or refused: fail the queue now. */
            struct lgn_auth_request queue[LGN_QUEUE]; unsigned queued=a->queued;
            memcpy(queue,a->queue,sizeof(queue)); lgn_wipe(a->queue,sizeof(a->queue)); a->queued=0;
            for (unsigned i=0;i<queued;i++) lgn_auth_reply(g,&queue[i],e,NULL,0);
            lgn_wipe(queue,sizeof(queue));
            return;
        }
    }
    a->active=a->queue[0];
    memmove(a->queue,a->queue+1,sizeof(a->queue[0])*(LGN_QUEUE-1));
    lgn_wipe(&a->queue[LGN_QUEUE-1],sizeof(a->queue[0]));
    a->queued--; a->busy=1;
    int e=auth_send(g);
    if (e) { lgn_note(g,"[logon] AUTH send failed: %d",e); lgn_auth_drop(g); }
}
/* Drains AUTH replies; a complete reply is dispatched, then the next request is sent. */
static void auth_receive(struct lgn *g)
{
    struct lgn_auth *a=&g->auth;
    struct ciuki_message m;
    for (unsigned burst=0;burst<64 && a->fd>=0;burst++) {
        int r=g->sys->recv(g->sys->ctx,a->fd,&m);
        if (r==-EAGAIN) break;
        if (r!=1) { lgn_auth_drop(g); return; }
        const uint8_t *h=m.data;
        uint32_t bytes=a32(h+36), total=a32(h+52), offset=a32(h+56); int32_t status=(int32_t)a32(h+48);
        if (!a->busy || m.fd_count || m.length<64 || m.length!=64+bytes || a32(h)!=CSC1 || a32(h+12)!=LGN_SERVICE_AUTH ||
            a32(h+16)!=a->active.operation || a32(h+20)!=2 || a64(h+24)!=a->id || a32(h+32)!=a->endpoint_generation ||
            offset!=a->got || (a->got && total!=a->total) || total>LGN_REPLY_MAX || bytes>total-offset ||
            status>0 || (status && total)) {
            lgn_wipe(&m,sizeof(m)); lgn_note(g,"[logon] malformed AUTH reply"); lgn_auth_drop(g); return;
        }
        if (!a->got) a->total=total;
        if (bytes) memcpy(a->reply+offset,h+64,bytes);
        a->got+=bytes; a->status=status;
        lgn_wipe(&m,sizeof(m));
        if (!status && a->got<a->total) continue;
        struct lgn_auth_request done=a->active;
        lgn_wipe(&a->active,sizeof(a->active)); a->busy=0;
        uint32_t got=a->got;
        lgn_auth_reply(g,&done,status,status ? NULL : a->reply,status ? 0 : got);
        lgn_wipe(a->reply,sizeof(a->reply)); lgn_wipe(&done,sizeof(done));
        lgn_auth_pump(g);
    }
    if (a->fd>=0 && !auth_live(g)) lgn_auth_drop(g);
}
void lgn_auth_poll(struct lgn *g) { if (g->auth.fd>=0) auth_receive(g); lgn_auth_pump(g); }
