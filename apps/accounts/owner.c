/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>

static int valid(const struct acct_owner_request *q)
{
    if (!q || q->count>32 || !q->transaction || !q->volume ||
        azero(q->sid,12) || (q->count && q->expected_accounts_generation>=q->transaction) ||
        azero(q->target_digest,32) || azero(q->manifest_digest,32)) return 0;
    for (unsigned i=0;i<q->count;++i) {
        const struct acct_owner_binding *b=&q->bindings[i];
        if ((i && q->bindings[i-1].uid>=b->uid) ||
            (b->uid!=500 && b->uid!=501 && (b->uid<1000 || b->uid==UINT32_MAX)) ||
            b->action<1 || b->action>3 || b->volume!=q->volume || b->locator ||
            !acct_profile_root_valid(b->root) ||
            (b->action==ACCT_OWNER_CREATE ? (b->key || b->generation) : (!b->key || !b->generation))) return 0;
        for (unsigned j=0;j<i;++j)
            if (!strcmp(b->root,q->bindings[j].root)) return 0;
    }
    return 1;
}
/* Canonical 160-byte header with operation and digest zero, and one
 * canonical 128-byte binding; the digest covers header+bindings in this form. */
static void encode_header(const struct acct_owner_request *q, uint8_t *p)
{
    memset(p,0,160);
    w32(p,1); w32(p+4,160); /* operation excluded from the canonical digest */
    w32(p+12,q->count);
    for (unsigned i=0;i<3;++i) w32(p+16+4*i,q->sid[i]);
    w64(p+32,q->volume); w64(p+40,q->transaction);
    w64(p+48,q->expected_accounts_generation); w64(p+56,q->expected_generation);
    memcpy(p+64,q->target_digest,32); memcpy(p+96,q->manifest_digest,32);
}
static void encode_binding(const struct acct_owner_binding *b, uint8_t *r)
{
    memset(r,0,128);
    w32(r,b->uid); w32(r+4,b->action); w64(r+8,b->key); w64(r+16,b->generation);
    w64(r+24,b->volume); w64(r+32,b->locator); w32(r+40,(uint32_t)strlen(b->root)+1);
    w32(r+44,1); memcpy(r+48,b->root,64);
}
int acct_owner_encode(const struct acct_owner_request *q, unsigned op, void *v, size_t cap, size_t *n)
{
    if (!valid(q) || op<1 || op>7 || !v || !n) return -EINVAL;
    if((op==5 || op==6)?q->count!=0:q->count==0) return -EINVAL;
    *n=160+128*q->count;
    if (cap<*n) return -ENOSPC;
    uint8_t *p=v; encode_header(q,p);
    for (unsigned i=0;i<q->count;++i) encode_binding(&q->bindings[i],p+160+128*i);
    sha256(p,*n,p+128); w32(p+8,op);
    return 0;
}
int acct_owner_decode(struct acct_owner_request *q, unsigned *op, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!q || !op || !p || n<160 || a32(p)!=1 || a32(p+4)!=160 || a32(p+28) ||
        a32(p+12)>32 || n!=160+128*a32(p+12) || a32(p+8)<1 || a32(p+8)>7) return -EINVAL;
    /* Decode into the caller's record (wiped on failure) and compare the
     * canonical form segment by segment with a streaming digest: no
     * 4 KiB canonical copy on the target stack. */
    struct acct_owner_request *t=q; memset(t,0,sizeof(*t)); t->count=a32(p+12);
    for(unsigned i=0;i<3;++i) t->sid[i]=a32(p+16+4*i);
    t->volume=a64(p+32); t->transaction=a64(p+40);
    t->expected_accounts_generation=a64(p+48); t->expected_generation=a64(p+56);
    memcpy(t->target_digest,p+64,32); memcpy(t->manifest_digest,p+96,32);
    int e=0;
    for (unsigned i=0;!e && i<t->count;++i) {
        struct acct_owner_binding *b=&t->bindings[i]; const uint8_t *r=p+160+128*i;
        if (!apadded(r+48,64) || a32(r+40)!=strlen((const char *)r+48)+1 ||
            a32(r+44)!=1 || !azero(r+112,16)) e=-EINVAL;
        b->uid=a32(r); b->action=a32(r+4); b->key=a64(r+8); b->generation=a64(r+16);
        b->volume=a64(r+24); b->locator=a64(r+32); memcpy(b->root,r+48,64);
    }
    unsigned operation=a32(p+8);
    if (!e && (!valid(t) || ((operation==5 || operation==6)?t->count!=0:t->count==0))) e=-EINVAL;
    if (!e) {
        uint8_t segment[160], digest[32]; struct sha256_ctx ctx; sha256_init(&ctx);
        encode_header(t,segment);
        /* Operation and digest fields are excluded from the canonical form. */
        if (memcmp(segment,p,8) || memcmp(segment+12,p+12,116)) e=-EIO;
        sha256_update(&ctx,segment,160);
        for (unsigned i=0;!e && i<t->count;++i) {
            encode_binding(&t->bindings[i],segment);
            if (!acct_equal_secret(segment,p+160+128*i,128)) e=-EIO;
            sha256_update(&ctx,segment,128);
        }
        sha256_final(&ctx,digest);
        if (!e && !acct_equal_secret(digest,p+128,32)) e=-EIO;
        acct_wipe(segment,sizeof(segment)); acct_wipe(&ctx,sizeof(ctx));
    }
    if (!e) { memcpy(t->digest,p+128,32); *op=operation; }
    else acct_wipe(t,sizeof(*t));
    return e;
}
int acct_owner_reply_encode(const struct acct_owner_reply *r, void *v, size_t cap, size_t *n)
{
    if (!r || !v || !n || r->count>32 || r->state>5 || ((r->state==0 || r->state==4) && (r->count || r->generation))) return -EINVAL;
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
    if (!r || !q || !p || n<96 || a32(p)!=1 || a32(p+4)!=96 || a32(p+8)>5 || a32(p+28) || a64(p+56) ||
        a32(p+12)>32 || n!=96+32*a32(p+12) || a64(p+32)!=q->volume || a64(p+40)!=q->transaction ||
        !acct_equal_secret(p+64,q->digest,32)) return -EIO;
    struct acct_owner_reply t={0}; t.state=a32(p+8); t.count=a32(p+12); t.generation=a64(p+48);
    if (((t.state==0 || t.state==4) && (t.count || t.generation)) || ((t.state==ACCT_OWNER_PENDING || t.state==ACCT_OWNER_RESERVED) && t.count!=q->count) ||
        (t.state==ACCT_OWNER_COMMITTED && t.count!=q->count) ||
        (t.state==ACCT_OWNER_ABORTED && (t.count!=q->count || t.generation<=q->expected_generation)) ||
        ((t.state==1 || t.state==2 || t.state==5) && t.generation<=q->expected_generation)) return -EIO;
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
    static const enum acct_boot_step steps[]={ ACCT_BOOT_RECOVER, ACCT_BOOT_PREPARE,
        ACCT_BOOT_COMMIT, ACCT_BOOT_ABORT, ACCT_BOOT_STATUS, ACCT_BOOT_CHECKPOINT,
        ACCT_BOOT_COMPACT, ACCT_BOOT_RESERVE };
    enum acct_boot_step step=op<sizeof(steps)/sizeof(*steps)?steps[op]:ACCT_BOOT_RECOVER;
    s->firstboot_step=step;
    struct call_frame { uint8_t request[ACCT_OWNER_BODY_MAX], reply[ACCT_OWNER_REPLY_MAX]; };
    struct call_frame *f=acct_frame_alloc(&s->kernel,sizeof(*f)); size_t n=0, m=0;
    if(!f) return acct_boot_result(s,step,-ENOMEM);
    int e=s->owner.authenticate(s->owner.ctx);
    if(!e) e=acct_owner_encode(q,op,f->request,sizeof(f->request),&n);
    if(!e) e=s->owner.call(s->owner.ctx,f->request,n,f->reply,sizeof(f->reply),&m);
    if(!e) e=acct_owner_reply_decode(r,q,f->reply,m);
    if(!e && (op==5 || op==6) && r->state!=ACCT_OWNER_CHECKPOINTED) e=-EIO;
    acct_frame_free(&s->kernel,f,sizeof(*f));
    return acct_boot_result(s,step,e);
}
