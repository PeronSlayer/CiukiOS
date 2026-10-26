/* SPDX-License-Identifier: MIT -- private LOGON relay/correlation core.
 * No UI, session creation or secure-leave bypass. user-03 binds the event loop. */
#include "accounts.h"
#include "wire.h"
#include <errno.h>

static void drop(struct acct_logon *l, struct acct_logon_attempt *a)
{
    if(a->ui.retained) l->kernel.release_peer(l->kernel.ctx,&a->ui);
    acct_wipe(a,sizeof(*a));
}
void acct_logon_clear(struct acct_logon *l)
{ for(unsigned i=0;i<8;++i) drop(l,&l->attempts[i]); }
static int current(struct acct_logon *l, const struct acct_challenge *c)
{
    uint32_t lg,ag,console,auth,uid;
    int e=l->kernel.role_generations(l->kernel.ctx,&lg,&ag); if(e) return e;
    if(lg!=1 || ag!=1) l->autologon_used=1;
    if(lg!=c->logon_generation || ag!=c->account_generation) return -EAGAIN;
    e=l->kernel.gate_state(l->kernel.ctx,c->session,&console,&auth,&uid); if(e) return e;
    return console==c->console && auth==c->authentication_epoch && (c->action!=3 || !c->uid || uid==c->uid)?0:-EAGAIN;
}
static struct acct_logon_attempt *find(struct acct_logon *l, uint64_t attempt)
{ for(unsigned i=0;i<8;++i) if(l->attempts[i].occupied && l->attempts[i].challenge.attempt==attempt) return &l->attempts[i]; return NULL; }
static int begin(struct acct_logon *l, const struct acct_subject *ui, const struct acct_begin *b, void *relay)
{
    if(!l || !relay || !l->kernel.gate_state || !l->kernel.subject_get || !l->kernel.same_link) return -EINVAL;
    struct acct_challenge c={.action=b->action,.session=b->session,
        .ui_generation=ui?ui->endpoint_generation:0,.flags=b->action==5?1:0};
    int e=l->kernel.role_generations(l->kernel.ctx,&c.logon_generation,&c.account_generation);
    if(e) return e;
    if(!c.logon_generation || c.logon_generation<l->attempt_generation) return -EAGAIN;
    if(c.logon_generation!=l->attempt_generation) {
        acct_logon_clear(l); l->attempt_generation=c.logon_generation; l->next_attempt=0;
    }
    uint64_t now=l->kernel.monotonic_ms(l->kernel.ctx); struct acct_logon_attempt *slot=NULL;
    for(unsigned i=0;i<8;++i) {
        struct acct_logon_attempt *a=&l->attempts[i];
        if(a->occupied && ((a->ui.retained && !l->kernel.peer_alive(l->kernel.ctx,&a->ui)) ||
            (!a->submitted && now-a->started>=5000) ||
            (ui && l->kernel.same_link(l->kernel.ctx,&a->ui,ui)==1))) drop(l,a);
        if(!a->occupied && !slot) slot=a;
    }
    if(!slot || l->next_attempt==UINT32_MAX) return -EAGAIN;
    c.attempt=((uint64_t)c.logon_generation<<32)|++l->next_attempt;
    uint32_t uid; e=l->kernel.gate_state(l->kernel.ctx,b->session,&c.console,&c.authentication_epoch,&uid);
    if(!e && b->action==3 && b->console!=c.console) e=-EAGAIN;
    if(!e) e=acct_begin_encode(b,1,relay,64);
    if(!e) e=acct_challenge_encode(&c,1,(uint8_t *)relay+64,64);
    if(e) return e;
    slot->occupied=1; slot->begin=*b; slot->challenge=c; slot->started=now;
    if(ui) { slot->ui=*ui; slot->ui.request_id=0; l->kernel.subject_get(l->kernel.ctx,&slot->ui); }
    return 0;
}
int acct_logon_begin(struct acct_logon *l, const struct acct_subject *ui, const void *v, size_t n, void *relay)
{
    if(!l || !ui || ui->role!=ACCT_ROLE_UI || ui->uid || ui->session_id || ui->service!=5 || !ui->endpoint_generation ||
        !l->kernel.peer_alive(l->kernel.ctx,ui)) return -EACCES;
    struct acct_begin b; int e=acct_begin_decode(&b,0,v,n); return e?e:begin(l,ui,&b,relay);
}
int acct_logon_autologon(struct acct_logon *l, int safe, int off, void *relay)
{
    uint32_t lg,ag; int e=l->kernel.role_generations(l->kernel.ctx,&lg,&ag); if(e) return e;
    if(l->autologon_used || safe || off || lg!=1 || ag!=1) { l->autologon_used=1; return -EACCES; }
    l->autologon_used=1;
    struct acct_begin b={.action=5,.selector=1,.uid=1000}; return begin(l,NULL,&b,relay);
}
int acct_logon_challenged(struct acct_logon *l, const struct acct_subject *account, const void *v, size_t n)
{
    struct acct_challenge c; int e=acct_challenge_decode(&c,0,v,n); if(e) return e;
    struct acct_logon_attempt *a=find(l,c.attempt); if(!a || a->resolved) return -EAGAIN;
    if(account->role!=3 || account->uid || account->service!=1 || account->role_generation!=c.account_generation ||
        !l->kernel.peer_alive(l->kernel.ctx,account)) return -EACCES;
    struct acct_challenge proposal=c; proposal.uid=0; proposal.accounts_generation=0;
    uint8_t p[64],expected[64]; acct_challenge_encode(&proposal,1,p,64); acct_challenge_encode(&a->challenge,1,expected,64);
    if(memcmp(p,expected,64) || (a->begin.selector==1 && a->begin.uid!=c.uid) || current(l,&c) ||
        (a->ui.retained && !l->kernel.peer_alive(l->kernel.ctx,&a->ui))) return -EAGAIN;
    a->challenge=c; a->resolved=1; return 0;
}
int acct_logon_submit(struct acct_logon *l, const struct acct_subject *ui, const void *v, size_t n, void *relay)
{
    const uint8_t *p=v;
    if(!p || n!=208 || !relay || !ui || ui->role!=2 || ui->service!=5 || !l->kernel.peer_alive(l->kernel.ctx,ui)) return -EACCES;
    struct acct_logon_attempt *a=find(l,a64(p+8));
    if(!a || !a->resolved || a->submitted || a->challenge.action==5 ||
        l->kernel.same_link(l->kernel.ctx,&a->ui,ui)!=1 || current(l,&a->challenge) ||
        l->kernel.monotonic_ms(l->kernel.ctx)-a->started>=5000) return -EAGAIN;
    uint8_t body[272]; acct_challenge_encode(&a->challenge,0,body,64); memcpy(body+64,p,208);
    struct acct_request q={0}; int e=acct_body_decode(1,a->challenge.action,body,272,a->challenge.accounts_generation,&q);
    if(!e && (a->begin.selector==1?q.name[0]!=0:!acct_name_equal(q.name,a->begin.name))) e=-EINVAL;
    if(!e) { memcpy(relay,body,272); a->submitted=1; }
    acct_wipe(&q,sizeof(q)); acct_wipe(body,sizeof(body)); return e;
}
int acct_logon_outcome(struct acct_logon *l, const struct acct_subject *account, uint64_t generation,
                       const void *v, size_t n, struct acct_auth_outcome *out, void *progress)
{
    if(!l || !account || !out || !progress) return -EINVAL;
    struct acct_auth_outcome o; int e=acct_outcome_decode(&o,0,v,n); if(e) return e;
    if(account->role!=3 || account->uid || account->service!=5 || account->role_generation!=o.challenge.account_generation ||
        !l->kernel.peer_alive(l->kernel.ctx,account)) return -EACCES;
    struct acct_logon_attempt *a=find(l,o.challenge.attempt); if(!a || !a->resolved || (!a->submitted && o.challenge.action!=5)) return -EAGAIN;
    uint8_t expected[64]; acct_challenge_encode(&a->challenge,0,expected,64);
    uint64_t expected_generation=o.disposition==3?o.resulting_generation:o.challenge.accounts_generation;
    if(memcmp(expected,v,64) || current(l,&o.challenge) || generation!=expected_generation ||
        (a->ui.retained && !l->kernel.peer_alive(l->kernel.ctx,&a->ui))) e=-EAGAIN;
    if(!e) {
        uint8_t *p=progress; memset(p,0,16); w32(p,1); w32(p+4,o.disposition); w32(p+8,(uint32_t)o.status); w32(p+12,o.retry_after_ms);
        *out=o;
    }
    drop(l,a); acct_wipe(&o,sizeof(o)); return e;
}
