/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>

static int valid(const struct acct_owner_request *q)
{
    if (!q || !q->count || q->count>32 || !q->transaction || !q->volume ||
        azero(q->sid,12) || q->expected_accounts_generation>=q->transaction ||
        azero(q->target_digest,32) || azero(q->manifest_digest,32)) return 0;
    for (unsigned i=0;i<q->count;++i) {
        const struct acct_owner_binding *b=&q->bindings[i];
        if ((i && q->bindings[i-1].uid>=b->uid) ||
            (b->uid!=500 && b->uid!=501 && (b->uid<1000 || b->uid==UINT32_MAX)) ||
            b->action<1 || b->action>3 || b->volume!=q->volume || !b->locator ||
            !acct_profile_root_valid(b->root) ||
            (b->action==ACCT_OWNER_CREATE ? (b->key || b->generation) : (!b->key || !b->generation))) return 0;
        for (unsigned j=0;j<i;++j)
            if (b->locator==q->bindings[j].locator || !strcmp(b->root,q->bindings[j].root)) return 0;
    }
    return 1;
}
int acct_owner_encode(const struct acct_owner_request *q, unsigned op, void *v, size_t cap, size_t *n)
{
    if (!valid(q) || op<1 || op>4 || !v || !n) return -EINVAL;
    *n=160+128*q->count;
    if (cap<*n) return -ENOSPC;
    uint8_t *p=v; memset(p,0,*n);
    w32(p,1); w32(p+4,160); /* operation excluded from the canonical digest */
    w32(p+12,q->count);
    for (unsigned i=0;i<3;++i) w32(p+16+4*i,q->sid[i]);
    w64(p+32,q->volume); w64(p+40,q->transaction);
    w64(p+48,q->expected_accounts_generation); w64(p+56,q->expected_generation);
    memcpy(p+64,q->target_digest,32); memcpy(p+96,q->manifest_digest,32);
    for (unsigned i=0;i<q->count;++i) {
        const struct acct_owner_binding *b=&q->bindings[i]; uint8_t *r=p+160+128*i;
        w32(r,b->uid); w32(r+4,b->action); w64(r+8,b->key); w64(r+16,b->generation);
        w64(r+24,b->volume); w64(r+32,b->locator); w32(r+40,(uint32_t)strlen(b->root)+1);
        w32(r+44,1); memcpy(r+48,b->root,64);
    }
    sha256(p,*n,p+128); w32(p+8,op);
    return 0;
}
int acct_owner_decode(struct acct_owner_request *q, unsigned *op, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!q || !op || !p || n<160 || a32(p)!=1 || a32(p+4)!=160 || a32(p+28) ||
        !a32(p+12) || a32(p+12)>32 || n!=160+128*a32(p+12) || a32(p+8)<1 || a32(p+8)>4) return -EINVAL;
    struct acct_owner_request t={0}; t.count=a32(p+12);
    for(unsigned i=0;i<3;++i) t.sid[i]=a32(p+16+4*i);
    t.volume=a64(p+32); t.transaction=a64(p+40);
    t.expected_accounts_generation=a64(p+48); t.expected_generation=a64(p+56);
    memcpy(t.target_digest,p+64,32); memcpy(t.manifest_digest,p+96,32);
    for (unsigned i=0;i<t.count;++i) {
        struct acct_owner_binding *b=&t.bindings[i]; const uint8_t *r=p+160+128*i;
        if (!apadded(r+48,64) || a32(r+40)!=strlen((const char *)r+48)+1 ||
            a32(r+44)!=1 || !azero(r+112,16)) return -EINVAL;
        b->uid=a32(r); b->action=a32(r+4); b->key=a64(r+8); b->generation=a64(r+16);
        b->volume=a64(r+24); b->locator=a64(r+32); memcpy(b->root,r+48,64);
    }
    uint8_t canonical[ACCT_OWNER_BODY_MAX]; size_t bytes;
    int e=acct_owner_encode(&t,a32(p+8),canonical,sizeof(canonical),&bytes);
    if (!e && !acct_equal_secret(canonical,p,n)) e=-EIO;
    if (!e) { memcpy(t.digest,p+128,32); *q=t; *op=a32(p+8); }
    acct_wipe(canonical,sizeof(canonical)); acct_wipe(&t,sizeof(t)); return e;
}
int acct_owner_reply_encode(const struct acct_owner_reply *r, void *v, size_t cap, size_t *n)
{
    if (!r || !v || !n || r->count>32 || r->state>3 || (!r->state && (r->count || r->generation))) return -EINVAL;
    *n=96+32*r->count; if(cap<*n) return -ENOSPC;
    uint8_t *p=v; memset(p,0,*n); w32(p,1); w32(p+4,96); w32(p+8,r->state); w32(p+12,r->count);
    for (unsigned i=0;i<3;++i) w32(p+16+4*i,r->sid[i]);
    w64(p+32,r->volume); w64(p+40,r->transaction); w64(p+48,r->generation); memcpy(p+64,r->digest,32);
    for (unsigned i=0;i<r->count;++i) {
        uint8_t *b=p+96+32*i; w32(b,r->results[i].uid); w32(b+4,r->results[i].index);
        w64(b+8,r->results[i].key); w64(b+16,r->results[i].generation); w32(b+24,r->results[i].action);
    }
    return 0;
}
int acct_owner_reply_decode(struct acct_owner_reply *r, const struct acct_owner_request *q, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!r || !q || !p || n<96 || a32(p)!=1 || a32(p+4)!=96 || a32(p+8)>3 || a32(p+28) || a64(p+56) ||
        a32(p+12)>32 || n!=96+32*a32(p+12) || a64(p+32)!=q->volume || a64(p+40)!=q->transaction ||
        !acct_equal_secret(p+64,q->digest,32)) return -EIO;
    struct acct_owner_reply t={0}; t.state=a32(p+8); t.count=a32(p+12); t.generation=a64(p+48);
    if ((!t.state && (t.count || t.generation)) || (t.state==ACCT_OWNER_PENDING && t.count!=q->count) ||
        (t.state==ACCT_OWNER_COMMITTED && t.count!=q->count) ||
        (t.state==ACCT_OWNER_ABORTED && t.count && t.count!=q->count) ||
        (t.state==ACCT_OWNER_ABORTED && (!t.count ? t.generation!=0 : t.generation<=q->expected_generation)) ||
        ((t.state==1 || t.state==2) && t.generation<=q->expected_generation)) return -EIO;
    for (unsigned i=0;i<3;++i) { t.sid[i]=a32(p+16+4*i); if(t.sid[i]!=q->sid[i]) return -EIO; }
    t.volume=q->volume; t.transaction=q->transaction; memcpy(t.digest,p+64,32);
    for (unsigned i=0;i<t.count;++i) {
        const uint8_t *b=p+96+32*i; const struct acct_owner_binding *old=&q->bindings[i];
        t.results[i].uid=a32(b); t.results[i].index=a32(b+4); t.results[i].key=a64(b+8);
        t.results[i].generation=a64(b+16); t.results[i].action=a32(b+24);
        if (a32(b+28) || t.results[i].uid!=old->uid || t.results[i].index!=i || t.results[i].action!=old->action ||
            !t.results[i].key || !t.results[i].generation ||
            (old->action!=ACCT_OWNER_CREATE && t.results[i].key!=old->key) ||
            t.results[i].generation!=(old->action==ACCT_OWNER_KEEP ? old->generation : t.generation)) return -EIO;
    }
    *r=t; return 0;
}
int acct_owner_call(struct acct_store *s, unsigned op, const struct acct_owner_request *q, struct acct_owner_reply *r)
{
    uint8_t request[ACCT_OWNER_BODY_MAX], reply[ACCT_OWNER_REPLY_MAX]; size_t n=0, m=0;
    int e=s->owner.authenticate(s->owner.ctx);
    if(!e) e=acct_owner_encode(q,op,request,sizeof(request),&n);
    if(!e) e=s->owner.call(s->owner.ctx,request,n,reply,sizeof(reply),&m);
    if(!e) e=acct_owner_reply_decode(r,q,reply,m);
    acct_wipe(request,sizeof(request)); acct_wipe(reply,sizeof(reply)); return e;
}
