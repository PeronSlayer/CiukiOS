/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "wire.h"
#include <errno.h>
#include <ciuki/sha256.h>

static int strings(const uint8_t *p)
{
    return apadded(p+48,32) && apadded(p+80,64) && apadded(p+144,64) &&
        a32(p+24)==strlen((const char *)p+48) && a32(p+28)==strlen((const char *)p+80) &&
        a32(p+32)==strlen((const char *)p+144) &&
        acct_password_valid((const char *)p+80,1) && acct_password_valid((const char *)p+144,1);
}
int acct_management_digest(const struct acct_request *q, uint8_t digest[32])
{
    if(!q || q->wire_operation<2 || q->wire_operation>5) return -EINVAL;
    uint8_t p[316]={0}; w32(p,q->wire_operation); w32(p+4,1);
    uint32_t flags=q->flags;
    if(q->operation==ACCT_CREATE) flags=(q->flags&1)|(q->groups==ACCT_ADMIN?2:0);
    w32(p+8,flags); w32(p+28,(uint32_t)strlen(q->name)); w32(p+32,(uint32_t)strlen(q->old_password));
    w32(p+36,(uint32_t)strlen(q->password)); w32(p+40,q->uid); w64(p+44,q->generation);
    memcpy(p+52,q->name,32); memcpy(p+84,q->old_password,64); memcpy(p+148,q->password,64); memcpy(p+212,q->picture,96);
    sha256(p,sizeof(p),digest); acct_wipe(p,sizeof(p)); return 0;
}
int acct_body_decode(unsigned service, unsigned op, const void *v, size_t n, uint64_t gen, struct acct_request *q)
{
    if(!q) return -EINVAL;
    struct acct_request t={0}; t.service=service; t.generation=gen; t.wire_operation=op;
    const uint8_t *p=v; int e=0;
    if((service==1 && op==1) || (service==2 && op==6)) {
        if(n) return -EINVAL;
        t.operation=ACCT_LIST; *q=t; return 0;
    }
    if(service!=1 && service!=2) return -EOPNOTSUPP;
    if(!op || (service==1?op>8:op>9)) return -EOPNOTSUPP; /* sec-user-04: AUTH 8 */
    if(service==1 && op==8) { /* sec-user-04: ENROLL_SOURCE, empty request */
        if(n) return -EINVAL;
        t.operation=ACCT_ENROLL_SOURCE; *q=t; return 0;
    }
    if(service==1 && op==7) {
        if(!p || n<72 || a32(p)!=1 || !a32(p+4) || a32(p+4)>16 || n!=32+40*a32(p+4) || a32(p+12) || a64(p+24)) return -EINVAL;
        t.operation=ACCT_ENROLL_PLAN; t.plan_count=a32(p+4); t.first_old_id=a32(p+8); t.generation=a64(p+16);
        if(t.generation!=gen) return -EINVAL;
        for(unsigned i=0;i<t.plan_count;++i) {
            const uint8_t *b=p+32+40*i; t.plan[i].old_id=a32(b);
            if(!apadded(b+4,32) || !acct_name_valid((const char *)b+4) || a32(b+36) ||
                (i && t.plan[i-1].old_id>=t.plan[i].old_id)) return -EINVAL;
            memcpy(t.plan[i].name,b+4,32);
        }
    } else if(service==1 && op==5) {
        if(!p || n!=128 || acct_begin_decode(&t.begin,1,p,64) || acct_challenge_decode(&t.challenge,1,p+64,64) ||
            t.begin.action!=t.challenge.action || t.begin.session!=t.challenge.session ||
            (t.begin.action==3 && t.begin.console!=t.challenge.console)) return -EINVAL;
        t.operation=ACCT_CHALLENGE;
    } else if(service==1) {
        if(!p || (op==6?n!=64:n!=272) || acct_challenge_decode(&t.challenge,0,p,64)) return -EINVAL;
        t.operation=op==2?ACCT_AUTH:op==3?ACCT_UNLOCK:op==4?ACCT_ENROLL_VERIFY:ACCT_AUTOLOGON;
        if(t.challenge.action!=(op==6?5:op)) return -EINVAL;
        t.uid=t.challenge.uid; t.generation=t.challenge.accounts_generation; t.attempt=t.challenge.attempt;
        t.session=t.challenge.session; t.console=t.challenge.console;
        if(gen!=t.generation) return -EINVAL;
        if(op!=6) {
            p+=64;
            if(a32(p)!=1 || a32(p+4) || !strings(p) || a64(p+8)!=t.attempt || a32(p+16)!=t.session ||
                a32(p+20)!=t.console || a32(p+36)!=t.uid || a64(p+40)!=t.generation || (op!=4 && a32(p+32))) return -EINVAL;
            memcpy(t.name,p+48,32); memcpy(t.old_password,p+80,64); memcpy(t.password,p+144,64);
            if(op!=4) { memcpy(t.password,t.old_password,64); memset(t.old_password,0,64); }
        }
    } else if(op==7 || op==9) {
        if(!p || n!=(op==7?32u:48u) || a32(p)!=1) return -EINVAL;
        if(op==7) {
            if(a32(p+4) || !a64(p+8) || !azero(p+16,16)) return -EINVAL;
            t.operation=ACCT_STATUS; t.client_operation_id=a64(p+8);
        } else {
            if(a32(p+4)<2 || a32(p+4)>5 || azero(p+8,32) || a64(p+40)) return -EINVAL;
            t.operation=ACCT_RESERVE; t.flags=a32(p+4); memcpy(t.request_digest,p+8,32);
        }
    } else {
        if(!p || n!=312 || a32(p)!=1 || !strings(p) || !apadded(p+208,96) ||
            a64(p+8) || a32(p+16) || a32(p+20) || a64(p+40)!=gen) return -EINVAL;
        t.flags=a32(p+4); t.uid=a32(p+36); t.client_operation_id=a64(p+304);
        memcpy(t.name,p+48,32); memcpy(t.old_password,p+80,64); memcpy(t.password,p+144,64); memcpy(t.picture,p+208,96);
        if((op==1 || op==8)?t.client_operation_id!=0:!t.client_operation_id) e=-EINVAL;
        if(op!=1 && op!=8 && !gen) e=-EINVAL;
        switch(op) {
        case 1: case 8:
            if(t.flags || t.password[0] || t.old_password[0] || t.picture[0] || (!!t.uid==!!t.name[0]) ||
                (t.name[0] && !acct_name_valid(t.name))) e=-EINVAL;
            t.operation=op==1?ACCT_LOOKUP:ACCT_PROFILE; break;
        case 5:
            if(t.flags || t.name[0] || t.password[0] || t.old_password[0] || t.picture[0] || !t.uid) e=-EINVAL;
            t.operation=ACCT_DELETE; break;
        case 2:
            if(t.flags || !t.uid || t.name[0] || t.picture[0] || !acct_password_valid(t.password,0)) e=-EINVAL;
            t.operation=ACCT_PASSWORD; t.self_password=1; break;
        case 3:
            if((t.flags&~3u) || t.uid || t.old_password[0] || t.picture[0] || !acct_name_valid(t.name) || !acct_password_valid(t.password,0)) e=-EINVAL;
            t.operation=ACCT_CREATE; t.groups=(t.flags&2)?ACCT_ADMIN:ACCT_STANDARD; t.flags&=1; break;
        case 4:
            t.selectors=t.flags&0x1f00;
            if(!t.uid || !t.selectors || (t.flags&~0x1f03u) || t.old_password[0] ||
                ((t.flags&1) && !(t.selectors&256)) || ((t.flags&2) && !(t.selectors&512)) ||
                (!(t.selectors&1024) && t.name[0]) || (!(t.selectors&2048) && t.password[0]) || (!(t.selectors&4096) && t.picture[0]) ||
                ((t.selectors&1024) && !acct_name_valid(t.name)) || ((t.selectors&2048) && !acct_password_valid(t.password,0))) e=-EINVAL;
            t.operation=ACCT_UPDATE; break;
        }
    }
    if(!e) { memcpy(t.confirm,t.password,64); *q=t; }
    acct_wipe(&t,sizeof(t)); return e;
}
int acct_public_encode(const struct acct_response *r, void *v, size_t cap, size_t *n)
{
    if(!r || !v || !n || r->count>18) return -EINVAL;
    *n=16+144*r->count; if(cap<*n) return -ENOSPC;
    uint8_t *p=v; memset(p,0,*n); w32(p,1); w32(p+4,r->count); w64(p+8,r->generation);
    for(unsigned i=0;i<r->count;++i) {
        const struct acct_public *r0=&r->records[i]; uint8_t *b=p+16+144*i;
        w32(b,r0->uid); w32(b+4,r0->groups); w32(b+8,r0->flags&(3u|ACCT_PUBLIC_BLANK_LOGON));
        memcpy(b+16,r0->name,32); memcpy(b+48,r0->picture,96);
    }
    return 0;
}
int acct_submit_body(struct acct_service *s, int endpoint, unsigned service, unsigned operation,
                     void *body, size_t n, uint64_t generation, uint64_t request_id, struct acct_response *out)
{
    struct acct_request q={0}; int e=acct_body_decode(service,operation,body,n,generation,&q);
    if(body) acct_wipe(body,n);
    q.request_id=request_id;
    if(!e) e=acct_submit(s,endpoint,&q,sizeof(q),out);
    else if(out) { memset(out,0,sizeof(*out)); out->error=e; }
    acct_wipe(&q,sizeof(q)); return e;
}
static void drop(struct acct_service *s, struct acct_exchange *x)
{
    if(x->body) { acct_wipe(x->body,x->total); s->store.kernel.deallocate(s->store.kernel.ctx,x->body,x->total); }
    /* Preserve replay high water until link revocation. */
    uint64_t id=x->request_id; int fd=x->endpoint; uint32_t ep=x->subject.endpoint_generation, service=x->service;
    if(x->occupied) {
        if(!x->transferred) s->store.kernel.exchange_end(s->store.kernel.ctx,&x->subject);
        s->store.kernel.release_peer(s->store.kernel.ctx,&x->subject);
    }
    memset(x,0,sizeof(*x)); x->endpoint=fd; x->request_id=id; x->subject.endpoint_generation=ep; x->service=service;
}
int acct_transport_cancel(struct acct_transport *t, struct acct_service *s, int fd)
{
    int result=0;
    for(unsigned i=0;i<8;++i) {
        struct acct_exchange *x=&t->exchanges[i];
        if(x->endpoint!=fd || !x->request_id) continue;
        struct acct_security_link link={0}; link.version=1; link.size=64;
        link.service=x->service; link.endpoint_fd=fd; link.endpoint_generation=x->subject.endpoint_generation;
        link.request_id_lo=(uint32_t)x->request_id; link.request_id_hi=(uint32_t)(x->request_id>>32);
        int error=s->store.kernel.service_link(s->store.kernel.ctx,ACCT_SERVICE_CANCEL,&link,NULL);
        if(error) result=error;
        if(x->occupied) drop(s,x);
    }
    return result;
}
/* Server-side teardown of a revoked link: forget its partial exchanges and
 * replay high water. CANCEL is a client operation; the kernel refuses it on
 * the server end, so this never calls service_link. */
void acct_transport_drop(struct acct_transport *t, struct acct_service *s, int fd)
{
    if(!t || !s) return;
    for(unsigned i=0;i<8;++i) {
        struct acct_exchange *x=&t->exchanges[i];
        if(x->endpoint!=fd) continue;
        if(x->occupied) drop(s,x);
        memset(x,0,sizeof(*x));
    }
}
int acct_frame(struct acct_transport *t, struct acct_service *s, int fd, const void *v, size_t n, struct acct_response *out)
{
    if(!t || !s || !v || n<64 || n>256) return -EINVAL;
    struct acct_kernel *k=&s->store.kernel; const uint8_t *p=v; struct acct_subject peer={0};
    int e=k->retain_peer(k->ctx,fd,&peer); if(e) return e;
    uint64_t now=k->monotonic_ms(k->ctx);
    for(unsigned i=0;i<8;++i) {
        struct acct_exchange *x=&t->exchanges[i];
        if(x->occupied && (!k->peer_alive(k->ctx,&x->subject) || now-x->started>=5000)) drop(s,x);
    }
    struct acct_exchange *x=NULL,*free_slot=NULL;
    for(unsigned i=0;i<8;++i) {
        struct acct_exchange *z=&t->exchanges[i];
        if(z->endpoint==fd && z->subject.endpoint_generation==peer.endpoint_generation) x=z;
        if(!z->occupied && !free_slot) free_slot=z;
    }
    if(a32(p)!=0x31435343 || a32(p+4)!=1 || a32(p+8)!=64 || a32(p+12)!=peer.service ||
        (peer.service!=1 && peer.service!=2) || a32(p+20)!=1 || !a64(p+24) ||
        a32(p+32)!=peer.endpoint_generation || a32(p+36)>192 || n!=64+a32(p+36) ||
        a32(p+48) || a32(p+52)>8192 || a32(p+60) || a32(p+56)>a32(p+52) ||
        a32(p+36)>a32(p+52)-a32(p+56) || (!a32(p+36) && a32(p+52))) e=-EINVAL;
    if(!e && x && !x->occupied && a64(p+24)<=x->request_id) e=-EINVAL;
    if(!e && (!x || !x->occupied)) {
        if(a32(p+56)) e=-EINVAL;
        else if(!x && !free_slot) e=-EAGAIN;
        else {
            if(!x) x=free_slot;
            e=k->exchange_begin(k->ctx,&peer,a64(p+24));
            if(e) { if(peer.retained) k->release_peer(k->ctx,&peer); return e; }
            memset(x,0,sizeof(*x)); x->occupied=1; x->endpoint=fd; x->subject=peer; peer.retained=0;
            x->subject.request_id=a64(p+24);
            x->request_id=a64(p+24); x->service=a32(p+12); x->operation=a32(p+16);
            x->generation=a64(p+40); x->total=a32(p+52); x->started=now;
            if(x->total) { x->body=k->allocate(k->ctx,x->total); if(!x->body) e=-ENOMEM; }
        }
    }
    if(!e && (x->request_id!=a64(p+24) || x->service!=a32(p+12) || x->operation!=a32(p+16) ||
        x->generation!=a64(p+40) || x->total!=a32(p+52) || x->used!=a32(p+56))) e=-EINVAL;
    if(!e) {
        if(a32(p+36)) memcpy(x->body+x->used,p+64,a32(p+36)); x->used+=a32(p+36);
        if(x->used==x->total) {
            if(!k->peer_alive(k->ctx,&x->subject)) e=-ESRCH;
            else {
                e=acct_submit_body(s,fd,x->service,x->operation,x->body,x->total,x->generation,x->request_id,out);
                if(!e) x->transferred=1; /* worker owns exchange until final drain */
            }
            drop(s,x);
        } else e=1; /* more fragments, no dispatch */
    }
    if(e<0 && x && x->occupied) drop(s,x);
    if(peer.retained) k->release_peer(k->ctx,&peer);
    return e;
}
int acct_event_encode(unsigned op, int reply, const struct acct_ui_event *e, void *v, size_t n)
{
    if(!e || !v || n!=32 || reply || op<2 || op>5 || op==4 || e->attempt) return -EINVAL;
    if(op==5 ? (e->action!=1 || !e->session || e->console) :
        (!e->console || (op==2?(e->action<1 || e->action>2 || !e->session):(e->action<3 || e->action>4)))) return -EINVAL;
    uint8_t *p=v; memset(p,0,32); w32(p,1); w32(p+4,e->action); w32(p+8,e->session); w32(p+12,e->console); return 0;
}
int acct_event_decode(unsigned op, int reply, struct acct_ui_event *e, const void *v, size_t n)
{
    const uint8_t *p=v;
    if(!e || !p || n!=32 || a32(p)!=1 || !azero(p+24,8)) return -EINVAL;
    struct acct_ui_event t={a32(p+4),a32(p+8),a32(p+12),a64(p+16)}; uint8_t canonical[32];
    int error=acct_event_encode(op,reply,&t,canonical,32); if(!error) *e=t; return error;
}
int acct_reply_body(unsigned service, unsigned operation, const struct acct_response *r, void *v, size_t cap, size_t *n)
{
    if(!r || !n || r->error>0) return -EINVAL;
    *n=0;
    if(r->error) return 0;
    if(service==1 && operation==8) return acct_source_encode(r,v,cap,n); /* sec-user-04 */
    if(r->body_bytes) {
        if(!v || r->body_bytes>sizeof(r->body) || cap<r->body_bytes) return -ENOSPC;
        memcpy(v,r->body,r->body_bytes); *n=r->body_bytes; return 0;
    }
    if((service==1 && (operation==1 || operation==7)) || (service==2 && (operation==1 || operation==6))) return acct_public_encode(r,v,cap,n);
    if(service!=2 || operation<2 || operation>5) return -EOPNOTSUPP;
    if(!v || cap<8 || !r->generation) return -EINVAL;
    w64(v,r->generation); *n=8; return 0;
}
int acct_reply_frame(const struct acct_subject *peer, unsigned operation, uint64_t id, uint64_t generation,
                      int status, const void *body, size_t total, size_t offset, void *v, size_t *n)
{
    if(!peer || !peer->retained || !peer->endpoint_generation || !id || !operation || !v || !n ||
        (peer->request_id && peer->request_id!=id) || status>0 || total>8192 || offset>total ||
        (status && total) || (total && !body) || (total && offset==total)) return -EINVAL;
    size_t fragment=total-offset; if(fragment>192) fragment=192;
    uint8_t *p=v; memset(p,0,64+fragment); w32(p,0x31435343); w32(p+4,1); w32(p+8,64);
    w32(p+12,peer->service); w32(p+16,operation); w32(p+20,2); w64(p+24,id);
    w32(p+32,peer->endpoint_generation); w32(p+36,(uint32_t)fragment); w64(p+40,generation);
    w32(p+48,(uint32_t)status); w32(p+52,(uint32_t)total); w32(p+56,(uint32_t)offset);
    if(fragment) memcpy(p+64,(const uint8_t *)body+offset,fragment);
    *n=64+fragment; return 0;
}
