/* SPDX-License-Identifier: MIT -- coherent OWNER snapshot / ACCOUNT marker */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>

struct validate_frame { uint8_t db[ACCT_FILE_MAX]; struct acct_owner_request q; struct acct_owner_reply r; };
static int validate_inner(const struct acct_store *s, const struct acct_db *d, const void *v, size_t n, uint64_t floor, struct validate_frame *f)
{
    const uint8_t *p=v;
    /* sec-own: OWNER emits CIUKIOCP v2 once creator-owned root records exist
     * (src/kernel/fs/owner_checkpoint.c): root count @140, then 160-byte root
     * records after the profiles. Version 1 keeps 140..160 zero. */
    unsigned version=p && n>=160 ? a32(p+8) : 0, roots=version==2 ? a32(p+140) : 0;
    if(!p || n<160 || memcmp(p,"CIUKIOCP",8) || (version!=1 && version!=2) || a32(p+12)!=160 || a32(p+16)>32 || a32(p+20)>256 ||
        memcmp(p+24,d->sid,12) || a32(p+36) || a64(p+40)!=s->volume || a64(p+48)!=floor ||
        floor>s->high_generation || a64(p+56)!=d->generation || a64(p+96)>a64(p+104) || a64(p+112)<a32(p+16) ||
        a64(p+120)<a32(p+16) || n<160+112*a32(p+16)) return -EIO;
    if(a64(p+128)<floor || a64(p+128)>s->high_generation || a32(p+136)<d->next_uid || a32(p+136)>s->high_uid ||
        (version==1 && !azero(p+140,20)) || (version==2 && (!roots || roots>256 || !azero(p+144,16) ||
        n<160+112*(size_t)a32(p+16)+160*(size_t)roots))) return -EIO;
    uint8_t *db=f->db, digest[32]; size_t bytes=0; int e=acct_encode(d,0,db,ACCT_FILE_MAX,&bytes);
    if(!e) { sha256(db,bytes,digest); if(memcmp(digest,p+64,32)) e=-EIO; }
    acct_wipe(db,ACCT_FILE_MAX); if(e) return e;
    unsigned matched=0;
    for(unsigned i=0;i<a32(p+16);++i) {
        const uint8_t *b=p+160+112*i; uint32_t uid=a32(b),action=a32(b+4);
        if((uid!=500 && uid!=501 && (uid<1000 || uid==UINT32_MAX)) || (i && a32(b-112)>=uid) || (action!=1 && action!=2) || !a64(b+8) || a64(b+8)>a64(p+112) ||
            !a64(b+16) || a64(b+16)>a64(p+96) || a64(b+24)!=s->volume || !a64(b+32) || a64(b+32)>a64(p+120) ||
            !apadded(b+40,64) || !acct_profile_root_valid((const char *)b+40) || a64(b+104)) return -EIO;
        for(unsigned j=0;j<i;++j) {
            const uint8_t *a=p+160+112*j;
            if(a64(a+8)==a64(b+8) || a64(a+32)==a64(b+32) || !strcmp((const char *)a+40,(const char *)b+40)) return -EIO;
        }
        int present=0; for(unsigned j=0;j<d->count;++j) present|=d->records[j].uid==uid;
        if(present) { if(action==2) return -EIO; ++matched; }
        else if(action!=2) return -EIO;
    }
    if(matched!=d->count) return -EIO;
    size_t offset=160+112*a32(p+16); uint64_t prior=floor;
    /* sec-own: creator-owned root records (v2): keys/locators within the
     * snapshot's watermarks, unique, single-component names; never profiles. */
    for(unsigned i=0;i<roots;++i,offset+=160) {
        const uint8_t *b=p+offset; uint32_t action=a32(b+4);
        if((action!=1 && action!=2) || !a64(b+8) || a64(b+8)>a64(p+112) || !a64(b+16) || a64(b+16)>a64(p+104) ||
            a64(b+24)!=s->volume || !a64(b+32) || a64(b+32)>a64(p+120) || !apadded(b+40,64) || b[40]!='/' || !b[41] ||
            strchr((const char *)b+41,'/') || a64(b+104) || azero(b+112,32) || !azero(b+144,16)) return -EIO;
        for(unsigned j=0;j<a32(p+16);++j) { const uint8_t *a=p+160+112*j; if(a64(a+8)==a64(b+8) || a64(a+32)==a64(b+32)) return -EIO; }
        for(unsigned j=0;j<i;++j) { const uint8_t *a=p+160+112*a32(p+16)+160*j; if(a64(a+8)==a64(b+8) || a64(a+32)==a64(b+32)) return -EIO; }
    }
    for(unsigned i=0;i<a32(p+20);++i) {
        if(n-offset<8) return -EIO;
        size_t qn=a32(p+offset),rn=a32(p+offset+4); offset+=8;
        if(qn>ACCT_OWNER_BODY_MAX || rn>ACCT_OWNER_REPLY_MAX || qn+rn>n-offset) return -EIO;
        struct acct_owner_request *q=&f->q; struct acct_owner_reply *r=&f->r; unsigned op;
        if(acct_owner_decode(q,&op,p+offset,qn) || acct_owner_reply_decode(r,q,p+offset+qn,rn) ||
            q->transaction<=prior || q->transaction>s->high_generation || q->volume!=s->volume || memcmp(q->sid,d->sid,12) ||
            (r->state!=ACCT_OWNER_COMMITTED && r->state!=ACCT_OWNER_ABORTED)) return -EIO;
        prior=q->transaction; offset+=qn+rn;
    }
    return offset==n?0:-EIO;
}
int acct_checkpoint_validate(const struct acct_store *s, const struct acct_db *d, const void *v, size_t n, uint64_t floor)
{
    struct validate_frame *f=acct_frame_alloc(&s->kernel,sizeof(*f)); if(!f) return -ENOMEM;
    int e=validate_inner(s,d,v,n,floor,f);
    acct_frame_free(&s->kernel,f,sizeof(*f)); return e;
}
static int marker_request(struct acct_store *s, const uint8_t *p, size_t n, struct acct_owner_request *q)
{
    unsigned op;
    if(n!=192 || acct_owner_decode(q,&op,p,160) || op!=5 || a64(p+160)>s->high_generation ||
        a32(p+168)>s->high_uid || !azero(p+172,20)) return -EIO;
    return 0;
}
static int verify_image(struct acct_store *s, const struct acct_db *d, const struct acct_owner_request *q)
{
    char path[128]; snprintf(path,sizeof(path),"/System/Security/Staging/g%016llx/checkpoint",(unsigned long long)q->transaction);
    uint8_t *p=s->kernel.allocate(s->kernel.ctx,ACCT_CHECKPOINT_MAX); if(!p) return -ENOMEM;
    size_t n; int e=s->io.read(s->io.ctx,path,p,ACCT_CHECKPOINT_MAX,&n); uint8_t digest[32];
    if(!e) { sha256(p,n,digest); if(memcmp(digest,q->manifest_digest,32)) e=-EIO; }
    if(!e && d->generation==q->expected_accounts_generation) e=acct_checkpoint_validate(s,d,p,n,q->transaction);
    acct_wipe(p,ACCT_CHECKPOINT_MAX); s->kernel.deallocate(s->kernel.ctx,p,ACCT_CHECKPOINT_MAX); return e;
}
static int publish_marker(struct acct_store *s, const uint8_t pending[192], const struct acct_owner_request *q)
{
    uint8_t old[192]; size_t n; uint64_t sequence=0;
    int e=acct_blob_read(s,"owner.checkpoint",old,sizeof(old),&n,&sequence);
    if(e && e!=-ENOENT) return e;
    if(!e) {
        struct acct_owner_request *previous=acct_frame_alloc(&s->kernel,sizeof(*previous)); if(!previous) return -ENOMEM;
        e=marker_request(s,old,n,previous);
        if(!e) {
            if(previous->transaction==q->transaction) e=memcmp(old,pending,192)?-EIO:1;
            else if(previous->transaction>q->transaction) e=-EIO;
        }
        acct_frame_free(&s->kernel,previous,sizeof(*previous));
        if(e) return e==1?0:e;
    }
    return acct_blob_write(s,"owner.checkpoint",pending,192,&sequence);
}
struct recover_frame { struct acct_owner_request q; struct acct_owner_reply reply; uint8_t p[192]; };
static int recover_inner(struct acct_store *s, const struct acct_db *d, struct recover_frame *f)
{
    uint8_t *p=f->p; size_t n; uint64_t sequence=0; struct acct_owner_request *qp=&f->q;
#define q (*qp)
    int e=acct_blob_read(s,"owner.checkpoint.pending",p,192,&n,&sequence);
    int pending=e!=-ENOENT;
    if(!pending) e=acct_blob_read(s,"owner.checkpoint",p,192,&n,&sequence);
    if(e==-ENOENT) return 0; if(e) return e;
    e=marker_request(s,p,n,&q); if(e) return e;
    uint64_t floor; unsigned count; e=s->owner.checkpoint_floor(s->owner.ctx,&floor,&count); if(e) return e;
    if(floor>q.transaction) return -EIO;
    if((pending || floor<q.transaction) && d->generation!=q.expected_accounts_generation) return -EIO;
    e=verify_image(s,d,&q); struct acct_owner_reply *replyp=&f->reply;
#define reply (*replyp)
    /* The exact request survives even a lost CHECKPOINT reply. Never build a
     * different candidate for this floor before completing the retained pair. */
    if(!e && pending) e=acct_owner_call(s,ACCT_OWNER_CHECKPOINT,&q,&reply);
    if(!e && pending) e=publish_marker(s,p,&q);
    if(!e) e=acct_owner_call(s,ACCT_OWNER_COMPACT,&q,&reply);
    if(!e) { e=s->owner.checkpoint_floor(s->owner.ctx,&floor,&count); if(!e && floor!=q.transaction) e=-EIO; }
    if(!e && pending) e=s->io.remove(s->io.ctx,"owner.checkpoint.pending");
    if(!e && pending) e=s->io.barrier(s->io.ctx);
    return e;
#undef q
#undef reply
}
int acct_owner_checkpoint_recover(struct acct_store *s, const struct acct_db *d)
{
    struct recover_frame *f=acct_frame_alloc(&s->kernel,sizeof(*f)); if(!f) return -ENOMEM;
    int e=recover_inner(s,d,f);
    acct_frame_free(&s->kernel,f,sizeof(*f)); return e;
}
struct checkpoint_frame { struct acct_owner_request q; uint8_t db[ACCT_FILE_MAX]; };
static int checkpoint_inner(struct acct_store *s, const struct acct_db *d, int force, struct checkpoint_frame *f)
{
    int e=acct_owner_checkpoint_recover(s,d); if(e) return e;
    uint64_t floor; unsigned count; e=s->owner.checkpoint_floor(s->owner.ctx,&floor,&count); if(e || (!force && count<768)) return e;
    uint8_t *image=s->kernel.allocate(s->kernel.ctx,ACCT_CHECKPOINT_MAX); if(!image) return -ENOMEM;
    struct acct_owner_request *qp=&f->q; size_t n=0; uint64_t next_floor=0;
#define q (*qp)
    e=s->owner.snapshot(s->owner.ctx,d,s->high_generation,s->high_uid,image,ACCT_CHECKPOINT_MAX,&n,&next_floor);
    if(!e && next_floor<=floor) e=-EAGAIN;
    if(!e && (n<160 || a64(image+128)!=s->high_generation || a32(image+136)!=s->high_uid)) e=-EIO;
    if(!e) e=acct_checkpoint_validate(s,d,image,n,next_floor);
    if(!e) {
        memcpy(q.sid,d->sid,12); q.volume=s->volume; q.transaction=next_floor; q.expected_accounts_generation=d->generation;
        e=s->owner.generation(s->owner.ctx,&q.expected_generation);
        uint8_t *db=f->db; size_t bytes; if(!e) e=acct_encode(d,0,db,ACCT_FILE_MAX,&bytes);
        if(!e) { sha256(db,bytes,q.target_digest); sha256(image,n,q.manifest_digest); }
        acct_wipe(db,ACCT_FILE_MAX);
    }
    char path[128]; snprintf(path,sizeof(path),"/System/Security/Staging/g%016llx/checkpoint",(unsigned long long)next_floor);
    if(!e) e=s->io.write(s->io.ctx,path,image,n);
    if(!e) e=s->io.flush(s->io.ctx,path);
    if(!e) e=s->io.barrier(s->io.ctx);
    acct_wipe(image,ACCT_CHECKPOINT_MAX); s->kernel.deallocate(s->kernel.ctx,image,ACCT_CHECKPOINT_MAX);
    uint8_t marker[192]={0}; size_t bytes;
    if(!e) e=acct_owner_encode(&q,5,marker,sizeof(marker),&bytes);
    uint64_t sequence=0;
    if(!e) { w64(marker+160,s->high_generation); w32(marker+168,s->high_uid);
        e=acct_blob_write(s,"owner.checkpoint.pending",marker,192,&sequence); }
    if(!e) e=acct_owner_checkpoint_recover(s,d);
    return e;
#undef q
}
int acct_owner_checkpoint(struct acct_store *s, const struct acct_db *d, int force)
{
    struct checkpoint_frame *f=acct_frame_alloc(&s->kernel,sizeof(*f)); if(!f) return -ENOMEM;
    int e=checkpoint_inner(s,d,force,f);
    acct_frame_free(&s->kernel,f,sizeof(*f)); return e;
}
