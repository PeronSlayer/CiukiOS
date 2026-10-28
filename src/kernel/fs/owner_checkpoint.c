/* Shared checkpoint images and two-party receipt compaction.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"
#define CHECKPOINT_MAX (160u+112u*OWNER_PROFILES+160u*OWNER_ROOTS+OWNER_RETAIN*(8u+OWNER_REQUEST_MAX+OWNER_REPLY_MAX))

static void checkpoint_record(uint8_t *p,const struct owner_record *r)
{
    memset(p,0,112); const struct owner_binding *b=&r->binding;
    fs_wr32(p,b->uid); fs_wr32(p+4,r->retired?OWNER_RETIRE:OWNER_CREATE);
    op64(p+8,b->key); op64(p+16,b->generation); op64(p+24,b->volume); op64(p+32,b->locator); memcpy(p+40,b->path,64);
}
int owner_checkpoint_image(struct owner_store *s,uint64_t accounts_generation,const uint8_t digest[32],uint64_t high_g,uint32_t high_uid,
                             void *v,size_t cap,size_t *bytes,uint64_t *floor)
{
    if(!s || !s->ready || !v || !bytes || !floor || !digest || ozero(digest,32) || s->active) return -EBUSY;
    if(high_g<s->transaction_high || high_uid<s->uid_high || accounts_generation>high_g || accounts_generation<s->committed_accounts_high) return -EIO;
    for(unsigned i=0;i<s->root_count;i++) if(s->roots[i].pending) return -EBUSY;
    unsigned skip=s->receipt_count>OWNER_RETAIN?s->receipt_count-OWNER_RETAIN:0, count=s->receipt_count-skip;
    uint64_t c=skip?s->receipts[skip-1]->transaction:s->floor;
    if(!c) return -EAGAIN;
    size_t n=160+112*s->profile_count+160*s->root_count;
    for(unsigned i=skip;i<s->receipt_count;i++) n+=8+s->receipts[i]->request_bytes+96+32*s->receipts[i]->count;
    if(cap<n) return -ENOSPC;
    uint8_t *p=v; memset(p,0,n); memcpy(p,"CIUKIOCP",8); fs_wr32(p+8,s->root_count?2:1); fs_wr32(p+12,160);
    fs_wr32(p+16,s->profile_count); fs_wr32(p+20,count); memcpy(p+24,s->sid,12); op64(p+40,s->volume);
    op64(p+48,c); op64(p+56,accounts_generation); memcpy(p+64,digest,32); op64(p+96,s->generation);
    op64(p+104,s->generation_high); op64(p+112,s->key_high); op64(p+120,s->locator_high); op64(p+128,high_g); fs_wr32(p+136,high_uid);
    fs_wr32(p+140,s->root_count); size_t at=160;
    for(unsigned i=0;i<s->profile_count;i++,at+=112) checkpoint_record(p+at,&s->profiles[i]);
    for(unsigned i=0;i<s->root_count;i++,at+=160) {
        checkpoint_record(p+at,&s->roots[i]); memcpy(p+at+112,s->roots[i].proof,32);
    }
    struct owner_request *q=oalloc(sizeof(*q)); if(!q) return -ENOMEM;
    int e=0;
    for(unsigned i=skip;!e && i<s->receipt_count;i++) {
        struct owner_receipt *r=s->receipts[i]; unsigned op; size_t reply_bytes=0;
        if(r->state!=OWNER_COMMITTED && r->state!=OWNER_ABORTED) { e=-EBUSY; break; }
        fs_wr32(p+at,r->request_bytes); fs_wr32(p+at+4,96+32*r->count); at+=8;
        memcpy(p+at,r->body,r->request_bytes); at+=r->request_bytes;
        e=owner_request_decode(q,&op,r->body,r->request_bytes);
        if(!e) e=owner_reply(q,r,r->state,p+at,cap-at,&reply_bytes);
        at+=reply_bytes;
    }
    ofree(q,sizeof(*q)); if(!e) { *bytes=at; *floor=c; } return e;
}
static int candidate_validate(struct owner_store *s,const struct owner_request *q)
{
    size_t cap=CHECKPOINT_MAX; uint8_t *p=oalloc(cap), *expected=oalloc(cap);
    if(!p || !expected) { ofree(p,cap); ofree(expected,cap); return -ENOMEM; }
    size_t n=0,m=0; uint64_t floor=0; uint8_t digest[32];
    int e=s->io.manifest(s->io.ctx,q->transaction,true,p,cap,&n);
    if(!e && (n<160 || n>cap)) e=-EIO;
    if(!e) { sha256(p,n,digest); if(memcmp(digest,q->manifest_digest,32)) e=-EIO; }
    if(!e) e=owner_checkpoint_image(s,q->accounts_generation,q->accounts_digest,ow64(p+128),fs_rd32(p+136),expected,cap,&m,&floor);
    if(!e && (m!=n || floor!=q->transaction || memcmp(p,expected,n))) e=-EIO;
    ofree(p,cap); ofree(expected,cap); return e;
}
int owner_checkpoint_call(struct owner_store *s,const struct owner_request *q,unsigned op,const void *wire)
{
    if(op!=OWNER_CHECKPOINT && op!=OWNER_COMPACT) return -EINVAL;
    if(s->active) return -EBUSY;
    if(q->transaction<s->floor) return -EIO;
    if(q->transaction==s->floor) return s->checkpoint[0] && !memcmp(s->checkpoint+128,q->digest,32)?0:-EIO;
    if(s->candidate && memcmp(s->checkpoint+128,q->digest,32)) return -EIO;
    if(q->owner_generation!=s->generation) return -EAGAIN;
    uint64_t current; uint8_t digest[32]; int e=s->io.account_current(s->io.ctx,&current,digest);
    if(!e && (current!=q->accounts_generation || memcmp(digest,q->accounts_digest,32))) e=-EIO;
    if(!e) e=candidate_validate(s,q);
    if(e) return e;
    if(op==OWNER_CHECKPOINT) {
        if(s->candidate) return 0;
        /* Keep the immutable candidate before publishing its marker. */
        size_t cap=CHECKPOINT_MAX, n=0; uint8_t *p=oalloc(cap); if(!p) return -ENOMEM;
        e=s->io.manifest(s->io.ctx,q->transaction,true,p,cap,&n);
        if(!e) e=s->io.write(s->io.ctx,4,p,n);
        if(!e) e=s->io.flush(s->io.ctx,4);
        if(!e) e=s->io.barrier(s->io.ctx);
        ofree(p,cap);
        if(!e) { memcpy(s->checkpoint,wire,160); fs_wr32(s->checkpoint+8,OWNER_CHECKPOINT); s->candidate=true; e=owner_persist(s); }
        if(e) s->ready=false; return e;
    }
    if(!s->candidate) return -EIO;
    /* The marker callback validates against the destination's durable ACCOUNT
     * state; a claimed bit in this request is never sufficient. */
    size_t cap=CHECKPOINT_MAX, n=0; uint8_t *p=oalloc(cap); if(!p) return -ENOMEM;
    e=s->io.read(s->io.ctx,4,p,cap,&n);
    if(!e && n<160) e=-EIO;
    if(!e) { sha256(p,n,digest); if(memcmp(digest,q->manifest_digest,32)) e=-EIO; }
    if(!e) e=s->io.checkpoint_marker(s->io.ctx,q,ow64(p+128),fs_rd32(p+136));
    if(!e) {
        if(s->account_high<ow64(p+128)) s->account_high=ow64(p+128);
        if(s->transaction_high<s->account_high) s->transaction_high=s->account_high;
        if(s->uid_high<fs_rd32(p+136)) s->uid_high=fs_rd32(p+136);
        unsigned skip=0; while(skip<s->receipt_count && s->receipts[skip]->transaction<=q->transaction) skip++;
        for(unsigned i=0;i<skip;i++) ofree(s->receipts[i],receipt_size(s->receipts[i]));
        memmove(s->receipts,&s->receipts[skip],(s->receipt_count-skip)*sizeof(*s->receipts)); s->receipt_count-=skip;
        s->floor=q->transaction; s->candidate=false; e=owner_persist(s);
    }
    ofree(p,cap); if(e) s->ready=false; return e;
}
int owner_checkpoint_recover(struct owner_store *s)
{
    if(!s->candidate) return 0;
    struct owner_request *q=oalloc(sizeof(*q)); if(!q) return -ENOMEM;
    uint8_t *p=oalloc(CHECKPOINT_MAX); if(!p) { ofree(q,sizeof(*q)); return -ENOMEM; }
    unsigned op; size_t n=0; uint8_t digest[32]; int e=owner_request_decode(q,&op,s->checkpoint,160);
    if(!e) e=s->io.read(s->io.ctx,4,p,CHECKPOINT_MAX,&n);
    if(!e && (n<160 || n>CHECKPOINT_MAX)) e=-EIO;
    if(!e) { sha256(p,n,digest); if(memcmp(digest,q->manifest_digest,32)) e=-EIO; }
    if(!e) {
        int marker=s->io.checkpoint_marker(s->io.ctx,q,ow64(p+128),fs_rd32(p+136));
        if(!marker) e=owner_checkpoint_call(s,q,OWNER_COMPACT,s->checkpoint);
        else if(marker!=-ENOENT) e=marker;
    }
    ofree(p,CHECKPOINT_MAX); ofree(q,sizeof(*q)); return e;
}
