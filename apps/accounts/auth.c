/* SPDX-License-Identifier: MIT -- frozen challenge/relay/outcome exchange */
#include "accounts.h"
#include "wire.h"
#include <errno.h>

int acct_begin_encode(const struct acct_begin *b, int internal, void *v, size_t n)
{
    if(!b || !v || n!=64 || b->action<2 || b->action>(internal?5u:4u) ||
        (b->selector!=1 && b->selector!=2) ||
        (b->selector==1?(!b->uid || !azero(b->name,32)):(b->uid || !apadded(b->name,32) || !acct_name_valid(b->name))) ||
        (b->action==3?(!b->session || !b->console):(b->session || b->console))) return -EINVAL;
    uint8_t *p=v; memset(p,0,64); w32(p,1); w32(p+4,b->action); w32(p+8,b->session); w32(p+12,b->console);
    w32(p+16,b->selector); w32(p+20,b->uid); memcpy(p+24,b->name,32); return 0;
}
int acct_begin_decode(struct acct_begin *b, int internal, const void *v, size_t n)
{
    const uint8_t *p=v;
    if(!b || !p || n!=64 || a32(p)!=1 || !azero(p+56,8)) return -EINVAL;
    struct acct_begin t={0}; t.action=a32(p+4); t.session=a32(p+8); t.console=a32(p+12);
    t.selector=a32(p+16); t.uid=a32(p+20); memcpy(t.name,p+24,32);
    uint8_t canonical[64]; int e=acct_begin_encode(&t,internal,canonical,64); if(!e) *b=t; return e;
}
int acct_challenge_encode(const struct acct_challenge *c, int proposal, void *v, size_t n)
{
    if(!c || !v || n!=64 || c->action<2 || c->action>5 || !c->attempt || !c->console || !c->authentication_epoch ||
        !c->logon_generation || !c->account_generation || (c->action==3?!c->session:c->session!=0) ||
        (c->action==5?(c->flags!=1 || c->ui_generation!=0):(c->flags || !c->ui_generation)) ||
        (proposal?(c->uid || c->accounts_generation):(!c->uid || c->uid==UINT32_MAX || !c->accounts_generation))) return -EINVAL;
    uint8_t *p=v; memset(p,0,64); w32(p,1); w32(p+4,c->action); w64(p+8,c->attempt); w32(p+16,c->uid);
    w32(p+20,c->session); w32(p+24,c->console); w32(p+28,c->authentication_epoch); w32(p+32,c->ui_generation);
    w32(p+36,c->logon_generation); w32(p+40,c->account_generation); w32(p+44,c->flags); w64(p+48,c->accounts_generation); return 0;
}
int acct_challenge_decode(struct acct_challenge *c, int proposal, const void *v, size_t n)
{
    const uint8_t *p=v;
    if(!c || !p || n!=64 || a32(p)!=1 || a64(p+56)) return -EINVAL;
    struct acct_challenge t={0}; t.action=a32(p+4); t.attempt=a64(p+8); t.uid=a32(p+16); t.session=a32(p+20);
    t.console=a32(p+24); t.authentication_epoch=a32(p+28); t.ui_generation=a32(p+32); t.logon_generation=a32(p+36);
    t.account_generation=a32(p+40); t.flags=a32(p+44); t.accounts_generation=a64(p+48);
    uint8_t canonical[64]; int e=acct_challenge_encode(&t,proposal,canonical,64); if(!e) *c=t; return e;
}
int acct_outcome_decode(struct acct_auth_outcome *out, int frame_status, const void *v, size_t n)
{
    const uint8_t *p=v;
    if(!out || frame_status || !p || n!=304) return -EINVAL;
    struct acct_auth_outcome t={0}; int e=acct_challenge_decode(&t.challenge,0,p,64); if(e) return e;
    t.status=(int32_t)a32(p+64); t.disposition=a32(p+68); t.retry_after_ms=a32(p+72); t.resulting_generation=a64(p+80);
    if(t.status>0 || t.retry_after_ms>1000 || a32(p+76) || a64(p+88)) return -EINVAL;
    if(t.disposition==4) { if(t.status>=0 || !azero(p+96,208)) return -EINVAL; }
    else if(t.disposition==2 || t.disposition==3) {
        if(t.status || t.challenge.action!=4 || !t.resulting_generation || !azero(p+96,208)) return -EINVAL;
    } else if(t.disposition==1) {
        if(t.status || t.retry_after_ms || t.challenge.action==4 || t.resulting_generation!=t.challenge.accounts_generation) return -EINVAL;
        const uint8_t *c=p+96,*r=p+160; unsigned uid=a32(c+8), count=a32(c+16), flags=a32(c+56);
        if(a32(c)!=1 || a32(c+4)!=64 || uid!=t.challenge.uid || a32(c+60) || (uid!=500 && uid!=501 && uid<1000) || (flags!=1 && !(flags==3 && uid==500)) ||
            a32(c+52)!=t.challenge.session || (count!=1 && count!=2) ||
            (uid==501?(count!=1 || a32(c+12)!=546):a32(c+12)!=545) || (uid==500 && count!=2) ||
            a32(c+20)!=(count==2?544:uid==501?546:545) || (count==2 && a32(c+24)!=545) ||
            !azero(c+20+4*count,4*(8-count))) return -EINVAL;
        if(a32(r)!=1 || a32(r+4)!=112 || a32(r+8)!=uid || a32(r+12) || !a64(r+16) || !a64(r+24) || !a64(r+32) ||
            a32(r+44) || !apadded(r+48,64) || !acct_profile_root_valid((const char *)r+48) || a32(r+40)!=strlen((const char *)r+48)+1 ||
            !apadded(p+272,32) || !acct_name_valid((const char *)p+272)) return -EINVAL;
        memcpy(t.credential,c,64); memcpy(t.profile,r,112); memcpy(t.name,p+272,32);
    } else return -EINVAL;
    *out=t; return 0;
}
static void drop_attempt(struct acct_service *s, struct acct_attempt *a)
{
    if(a->occupied) s->store.kernel.release_peer(s->store.kernel.ctx,&a->logon);
    acct_wipe(a,sizeof(*a));
}
void acct_attempts_clear(struct acct_service *s)
{ for(unsigned i=0;i<8;++i) drop_attempt(s,&s->attempts[i]); }
static int live(struct acct_service *s, const struct acct_subject *p, const struct acct_challenge *c, uint32_t *uid)
{
    struct acct_kernel *k=&s->store.kernel; uint32_t lg=0,ag=0;
    int e=k->role_generations(k->ctx,&lg,&ag);
    if(e) return e;
    if(lg!=1 || ag!=1) s->autologon_used=1;
    if(p->role!=ACCT_ROLE_LOGON || p->uid || p->session_id || p->role_generation!=c->logon_generation ||
        lg!=c->logon_generation || ag!=c->account_generation || !k->peer_alive(k->ctx,p)) return -EAGAIN;
    return k->auth_binding(k->ctx,p,c->ui_generation,c->session,c->console,c->authentication_epoch,lg,ag,uid);
}
static struct acct_db *catalogue(struct acct_service *s, unsigned action)
{ return action==4 && s->enrollment.generation?&s->enrollment.draft:&s->db; }
static const struct acct_record *record(const struct acct_db *d, const struct acct_begin *b)
{
    for(unsigned i=0;i<d->count;++i)
        if(b->selector==1?d->records[i].uid==b->uid:acct_name_equal(d->records[i].name,b->name)) return &d->records[i];
    return NULL;
}
static int issue(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    struct acct_request *q=&job->request; struct acct_challenge c=q->challenge;
    if(!s->available && c.action!=4) return -EIO;
    uint32_t locked=0; int e=live(s,&job->subject,&c,&locked); if(e) return e;
    if(c.attempt<=s->high_attempt) return -EAGAIN;
    s->high_attempt=c.attempt;
    struct acct_db *d=catalogue(s,c.action); const struct acct_record *r=record(d,&q->begin);
    struct acct_attempt *slot=NULL; uint64_t now=s->store.kernel.monotonic_ms(s->store.kernel.ctx);
    for(unsigned i=0;i<8;++i) {
        struct acct_attempt *a=&s->attempts[i];
        if(a->occupied && (!s->store.kernel.peer_alive(s->store.kernel.ctx,&a->logon) || now-a->started>=5000 ||
            (a->challenge.ui_generation==c.ui_generation && a->challenge.logon_generation==c.logon_generation))) drop_attempt(s,a);
        if(a->occupied && a->challenge.attempt==c.attempt) return -EAGAIN;
        if(!a->occupied && !slot) slot=a;
    }
    if(!r || (c.action==4 ? (!s->enrollment.generation && !(r->legacy_id && (r->flags&ACCT_CHANGE_REQUIRED))) :
        (!(r->flags&ACCT_ENABLED) && !(r->uid==500 && job->subject.safe_mode && (r->flags&ACCT_PASSWORD_SET))))) return -EACCES;
    if(c.action==3 && locked!=r->uid) return -EACCES;
    if(c.action==5) {
        struct acct_bootstrap b;
        if(q->begin.selector!=1 || q->begin.uid!=1000 || c.logon_generation!=1 || c.account_generation!=1 || s->autologon_used ||
            acct_boot_read(&s->store,&b) || !acct_autologon_eligible(&s->db,&b,job->subject.safe_mode,0,0)) return -EACCES;
        s->autologon_used=1;
    }
    if(!slot) return -EAGAIN;
    c.uid=r->uid; c.accounts_generation=c.action==4 && s->enrollment.generation?s->enrollment.generation:d->generation;
    e=acct_challenge_encode(&c,0,out->body,64); if(e) return e;
    slot->occupied=1; slot->begin=q->begin; slot->challenge=c; slot->started=now; slot->logon=job->subject;
    slot->logon.request_id=0; s->store.kernel.subject_get(s->store.kernel.ctx,&slot->logon);
    out->body_bytes=64; return 0;
}
int acct_auth_result(struct acct_service *s, struct acct_job *job, struct acct_response *out, int status,
                     unsigned disposition, const struct acct_record *r)
{
    uint8_t body[304]={0}; int e=acct_challenge_encode(&job->request.challenge,0,body,64); if(e) return e;
    w32(body+64,(uint32_t)status); w32(body+68,disposition); w32(body+72,status==-EACCES?1000:0);
    uint64_t gen=s->enrollment.generation && disposition==2?s->enrollment.generation:s->db.generation;
    w64(body+80,gen);
    if(r && !status && disposition==1) {
        char root[64]={0}; struct acct_owner_binding b={0};
        e=s->store.owner.binding(s->store.owner.ctx,s->store.volume,r->uid,root,&b);
        if(e || !b.key || !b.generation || !acct_profile_root_valid(root)) return -EIO;
        uint8_t *c=body+96; w32(c,1); w32(c+4,64); w32(c+8,r->uid); w32(c+12,r->groups==ACCT_GUEST?546:545);
        w32(c+16,r->groups==ACCT_ADMIN?2:1); w32(c+20,r->groups==ACCT_ADMIN?544:r->groups==ACCT_GUEST?546:545);
        if(r->groups==ACCT_ADMIN) w32(c+24,545);
        w32(c+52,job->request.challenge.session); w32(c+56,1u|((r->uid==500 && !(r->flags&ACCT_ENABLED) && job->subject.safe_mode)?2u:0u));
        uint8_t *p=body+160; w32(p,1); w32(p+4,112); w32(p+8,r->uid); w64(p+16,s->store.volume);
        w64(p+24,b.generation); w64(p+32,b.key); w32(p+40,(uint32_t)strlen(root)+1); memcpy(p+48,root,64); memcpy(body+272,r->name,32);
    }
    e=s->store.kernel.auth_outcome(s->store.kernel.ctx,&job->subject,job->request.challenge.attempt,0,body,sizeof(body));
    if(!e) { w32(out->body,1); w32(out->body+4,disposition); w32(out->body+8,(uint32_t)status);
        w32(out->body+12,status==-EACCES?1000:0); out->body_bytes=16; out->retry_after_ms=status==-EACCES?1000:0; }
    acct_wipe(body,sizeof(body)); return e;
}
int acct_auth_execute(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    if(s->faulted) { acct_attempts_clear(s); return -EIO; }
    if(job->request.operation==ACCT_CHALLENGE) return issue(s,job,out);
    if(job->request.operation==ACCT_ENROLL_PLAN || job->request.operation==ACCT_ENROLL_SOURCE) /* sec-user-04 */
        return acct_enrollment_execute(s,job,out);
    struct acct_request *q=&job->request; struct acct_attempt saved={0};
    for(unsigned i=0;i<8;++i) {
        struct acct_attempt *a=&s->attempts[i];
        if(a->occupied && a->challenge.attempt==q->challenge.attempt) {
            saved=*a; memset(a,0,sizeof(*a)); break;
        }
    }
    if(!saved.occupied) return -EAGAIN;
    uint8_t expected[64], got[64]; acct_challenge_encode(&saved.challenge,0,expected,64);
    int e=acct_challenge_encode(&q->challenge,0,got,64); uint32_t locked=0;
    if(!e && (memcmp(expected,got,64) || s->store.kernel.same_link(s->store.kernel.ctx,&saved.logon,&job->subject)!=1 ||
        s->store.kernel.monotonic_ms(s->store.kernel.ctx)-saved.started>=5000)) e=-EAGAIN;
    if(!e) e=live(s,&job->subject,&q->challenge,&locked);
    struct acct_db *d=catalogue(s,q->challenge.action); const struct acct_record *r=record(d,&saved.begin);
    uint64_t gen=q->challenge.action==4 && s->enrollment.generation?s->enrollment.generation:d->generation;
    if(!e && (q->generation!=gen || !r || r->uid!=q->challenge.uid || q->uid!=r->uid ||
        (saved.begin.selector==1?q->name[0]!=0:!acct_name_equal(q->name,saved.begin.name)) ||
        (q->challenge.action==3 && locked!=r->uid))) e=-EAGAIN;
    if(!e && q->operation==ACCT_ENROLL_VERIFY) e=acct_enrollment_execute(s,job,out);
    else if(!e) {
        int eligible=(r->flags&ACCT_ENABLED) || (r->uid==500 && job->subject.safe_mode && (r->flags&ACCT_PASSWORD_SET));
        int match=acct_verify(eligible?r:NULL,q->operation==ACCT_AUTOLOGON?"":q->password);
        if(!s->store.kernel.peer_alive(s->store.kernel.ctx,&job->subject)) e=-ESRCH;
        else e=eligible && match && !(r->flags&ACCT_CHANGE_REQUIRED)?0:-EACCES;
        if(!e) e=live(s,&job->subject,&q->challenge,&locked);
        if(!e) e=acct_auth_result(s,job,out,0,1,r);
    }
    if(e && s->store.kernel.peer_alive(s->store.kernel.ctx,&job->subject)) {
        int result=acct_auth_result(s,job,out,e,4,NULL);
        if(e==-EACCES) { uint64_t now=s->store.kernel.monotonic_ms(s->store.kernel.ctx); s->retry_at=now>UINT64_MAX-1000?UINT64_MAX:now+1000; }
        e=result;
    }
    drop_attempt(s,&saved); return e;
}
int acct_logon_session_lock(const struct acct_kernel *k, const struct acct_subject *p, const void *v, size_t n, void *progress)
{
    struct acct_ui_event event;
    if(!k || !p || !progress || acct_event_decode(5,0,&event,v,n)) return -EINVAL;
    if(p->role!=ACCT_ROLE_DESKTOP || !p->session_id || p->session_id!=event.session || !k->peer_alive(k->ctx,p)) return -EACCES;
    int e=k->session_lock(k->ctx,p,event.session); if(e) return e;
    uint8_t *b=progress; memset(b,0,16); w32(b,1); return 0;
}
