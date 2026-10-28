/* Retained-subject OWNER service and recovery. All persistent transitions use
 * owner_persist; failures close admission until recovered from durable evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"
#include "path.h"
#include "fat.h"
#include <ciuki/security.h>
static struct owner_store *installed_owners;

void owner_object_proof(const struct fat_volume *v,const struct fat_entry *entry,uint8_t proof[32])
{
    /* Directories retain their allocation root through rename and metadata
     * updates. Files retain their namespace slot through chain replacement and
     * all metadata updates; owner_root_finish records the new slot on rename.
     * Never use file clusters, length, attributes or user-settable birth times.
     * The sealed key/generation and namespace protocol fence deletion/reuse;
     * this hash alone is not an ownership credential. */
    uint8_t data[32]={0}; memcpy(data,"OWN2",4); fs_wr32(data+4,v->volume_serial);
    fs_wr32(data+8,(uint32_t)v->start); fs_wr32(data+12,(uint32_t)(v->start>>32));
    data[16]=entry->attr&FAT_ATTR_DIR;
    if(data[16]) fs_wr32(data+20,entry->first);
    else { fs_wr32(data+20,entry->parent); fs_wr32(data+24,entry->index); }
    sha256(data,sizeof(data),proof);
}

static bool enter(struct owner_store *s)
{ bool expected=false; return __atomic_compare_exchange_n(&s->busy,&expected,true,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
static void leave(struct owner_store *s) { __atomic_store_n(&s->busy,false,__ATOMIC_RELEASE); }
static struct owner_record *profile_find(struct owner_store *s,uint32_t uid)
{
    for(unsigned i=0;i<s->profile_count;i++) if(s->profiles[i].binding.uid==uid) return &s->profiles[i];
    return 0;
}
static int occupied_records(const struct owner_store *s,const char *root)
{
    if(s->root_key && ((s->root_old[0] && path_equal(root,s->root_old)) ||
                      (s->root_new[0] && path_equal(root,s->root_new)))) return 1;
    for(unsigned i=0;i<s->profile_count;i++) if(path_equal(root,s->profiles[i].binding.path)) return 1;
    for(unsigned i=0;i<s->root_count;i++) if(path_equal(root,s->roots[i].binding.path)) return 1;
    if(s->active) for(unsigned i=0;i<s->active->count;i++) if(path_equal(root,s->pending[i].binding.path)) return 1;
    return 0;
}
int owner_root_occupied(const struct owner_store *s,uint64_t volume,const char *root)
{
    if(!s || !root || !s->ready || volume!=s->volume || !s->io.occupied) return -EIO;
    int occupied=occupied_records(s,root);
    return occupied?occupied:s->io.occupied(s->io.ctx,root);
}
int owner_profile(const struct owner_store *s,uint32_t uid,struct owner_record *out)
{
    if(!s || !out || !s->ready || s->busy) return -EIO;
    for(unsigned i=0;i<s->profile_count;i++) if(s->profiles[i].binding.uid==uid) {
        if(s->profiles[i].retired || s->profiles[i].pending) return -EACCES;
        *out=s->profiles[i]; return 0;
    }
    return -ENOENT;
}
int owner_label(const struct owner_store *s,uint64_t locator,struct owner_record *out)
{
    if(!s || !out || !s->ready || !locator) return -EIO; /* sec-own: label reads do not wait for a transaction */
    for(unsigned i=0;i<s->profile_count+s->root_count;i++) {
        const struct owner_record *r=i<s->profile_count?&s->profiles[i]:&s->roots[i-s->profile_count];
        if(r->binding.locator==locator) {
            /* Retired profiles remain private to the retired uid. They are
             * never eligible for SESSION_CREATE. Retired creator entries are tombstones. */
            if(r->pending || (i>=s->profile_count && r->retired)) return -EACCES;
            *out=*r; return 0;
        }
    }
    return -EIO;
}
int owner_security_bind(struct owner_store *s)
{
    if(!s) { installed_owners=0; return 0; }
    if(!s || !s->ready || s->busy) return -EIO;
    if(installed_owners && installed_owners!=s) return -EBUSY;
    installed_owners=s; return 0;
}
void owner_security_unbind(struct owner_store *s) { if(installed_owners==s) installed_owners=0; }
int security_owner_verify(const struct ciuki_security_profile *p)
{
    struct owner_record r;
    if(!p || !installed_owners || owner_profile(installed_owners,p->uid,&r)) return -EIO;
    return r.binding.volume==((uint64_t)p->volume_id_hi<<32|p->volume_id_lo) &&
        r.binding.key==((uint64_t)p->owner_key_hi<<32|p->owner_key_lo) &&
        r.binding.generation==((uint64_t)p->owner_generation_hi<<32|p->owner_generation_lo) &&
        p->path_bytes==strlen(r.binding.path)+1 && !memcmp(p->path,r.binding.path,64)?0:-EIO;
}
int owner_intent(const struct owner_store *s,const struct owner_request *q)
{ return s->io.account_intent(s->io.ctx,q); }
int owner_pending_records(const struct owner_store *s,const struct owner_request *q,const struct owner_receipt *r,struct owner_record *out)
{
    if(s->active!=r || r->count!=q->count) return -EIO;
    memcpy(out,s->pending,q->count*sizeof(*out)); return 0;
}
int owner_save_records(struct owner_store *s,const struct owner_request *q,struct owner_receipt *r)
{
    if(s->active!=r) return -EIO;
    for(unsigned i=0;i<q->count;i++) {
        struct owner_record *old=profile_find(s,q->bindings[i].uid);
        if(q->bindings[i].action==OWNER_CREATE) {
            if(old || s->profile_count==32) return -EIO;
            unsigned at=0; while(at<s->profile_count && s->profiles[at].binding.uid<q->bindings[i].uid) at++;
            memmove(&s->profiles[at+1],&s->profiles[at],(s->profile_count-at)*sizeof(*old));
            s->profiles[at]=s->pending[i]; s->profiles[at].pending=false; s->profile_count++;
        } else {
            if(!old) return -EIO;
            if(q->bindings[i].action==OWNER_RETIRE) { *old=s->pending[i]; old->retired=true; old->pending=false; }
        }
    }
    s->generation=r->generation; s->committed_accounts_high=q->transaction; memcpy(s->committed_accounts_digest,q->accounts_digest,32);
    r->state=OWNER_COMMITTED; s->active=0; memset(s->pending,0,sizeof(s->pending));
    return owner_persist(s);
}
/* sec-sysvc (lead decision, first-boot uid recovery): a power cut between
 * OWNER's durable RESERVE and ACCOUNT's intent write leaves an orphan that the
 * next boot aborts, which raises uid_high past the ready/Setup recipe's uid.
 * Such a uid may be reserved again only while the store's complete history
 * proves that nothing was ever published: no committed generation or account
 * batch, no checkpoint floor or candidate (every receipt since creation is
 * retained), no profile record, no batch in flight, every receipt ABORTED,
 * and at least one of them named this uid. Every other uid below the
 * watermark stays retired (contract 3.5). Creator-root records may name the
 * uid (the interim session's own entries); they are not profile ownership. */
static bool first_boot_retry(const struct owner_store *s,uint32_t uid)
{
    if(s->generation || s->committed_accounts_high || s->floor || s->candidate || s->checkpoint[0] ||
       s->profile_count || s->active || s->root_key) return false;
    bool named=false;
    for(unsigned i=0;i<s->receipt_count;i++) {
        struct owner_receipt *r=s->receipts[i];
        if(r->state!=OWNER_ABORTED) return false;
        for(unsigned j=0;j<r->count;j++) if(fs_rd32(receipt_results(r)+32*j)==uid) named=true;
    }
    return named;
}
static int validate_bindings(struct owner_store *s,const struct owner_request *q)
{
    unsigned creates=0;
    for(unsigned i=0;i<q->count;i++) {
        const struct owner_binding *b=&q->bindings[i]; struct owner_record *old=profile_find(s,b->uid);
        if(b->action==OWNER_CREATE) {
            int occupied=occupied_records(s,b->path);
            if(old || occupied) return occupied<0?occupied:-EEXIST;
            if(b->uid>=1000 && b->uid<s->uid_high && !first_boot_retry(s,b->uid)) return -EIO; /* sec-sysvc */
            creates++;
        } else if(!old || old->retired || old->binding.key!=b->key || old->binding.generation!=b->generation ||
                   memcmp(old->binding.path,b->path,64)) return -EIO;
    }
    if(s->profile_count+creates>32) return -ENOSPC;
    if(s->generation_high==UINT64_MAX || s->key_high>UINT64_MAX-creates || s->locator_high>UINT64_MAX-creates) return -EOVERFLOW;
    return 0;
}
static int reserve_batch(struct owner_store *s,const struct owner_request *q,const void *wire,size_t n,struct owner_receipt **out)
{
    if(s->candidate || s->receipt_count>=OWNER_COMPACT_AT) return -EAGAIN;
    if(s->active || s->root_key) return -EBUSY;
    if(q->transaction<=s->transaction_high || q->transaction<=s->floor) return -EIO;
    if(q->owner_generation!=s->generation) return -EAGAIN;
    uint64_t account_generation; uint8_t digest[32];
    int e=s->io.account_current(s->io.ctx,&account_generation,digest);
    if(!e && account_generation!=q->accounts_generation) e=-EAGAIN;
    if(!e && (account_generation<s->committed_accounts_high ||
        (account_generation==s->committed_accounts_high && account_generation && memcmp(digest,s->committed_accounts_digest,32)))) e=-EIO;
    if(!e) e=validate_bindings(s,q);
    if(!e) e=owner_manifest_validate(s,q);
    if(e) return e;
    size_t allocation=sizeof(struct owner_receipt)+n+32*q->count;
    struct owner_receipt *r=oalloc(allocation); if(!r) return -ENOMEM;
    r->transaction=q->transaction; r->generation=s->generation_high+1; r->state=OWNER_RESERVED; r->count=q->count; r->request_bytes=(uint32_t)n;
    memcpy(r->body,wire,n); fs_wr32(r->body+8,OWNER_RESERVE);
    uint64_t key=s->key_high, locator=s->locator_high;
    for(unsigned i=0;i<q->count;i++) {
        struct owner_record *p=&s->pending[i]; const struct owner_binding *b=&q->bindings[i];
        if(b->action==OWNER_CREATE) { memset(p,0,sizeof(*p)); p->binding=*b; p->binding.key=++key; p->binding.locator=++locator; }
        else *p=*profile_find(s,b->uid);
        p->binding.action=b->action; p->pending=true;
        if(b->action!=OWNER_KEEP) p->binding.generation=r->generation;
        uint8_t *result=receipt_results(r)+32*i;
        fs_wr32(result,b->uid); fs_wr32(result+4,i); op64(result+8,p->binding.key); op64(result+16,p->binding.generation); fs_wr32(result+24,b->action);
    }
    /* Seal under the backend's namespace reservation before durable RESERVE.
     * Failure may retain fences; release only a demonstrably unpublished batch. */
    e=s->io.reserve(s->io.ctx,q,s->pending,false);
    if(!e) e=owner_manifest_validate(s,q); /* hash again after the lease is sealed */
    if(e) { s->io.release(s->io.ctx,q,false); memset(s->pending,0,sizeof(s->pending)); ofree(r,allocation); return e; }
    s->key_high=key; s->locator_high=locator; s->generation_high=r->generation; s->transaction_high=q->transaction;
    if(s->account_high<q->transaction) s->account_high=q->transaction;
    for(unsigned i=0;i<q->count;i++) if(q->bindings[i].uid>=1000 && s->uid_high<=q->bindings[i].uid) s->uid_high=q->bindings[i].uid+1;
    s->receipts[s->receipt_count++]=r; s->active=r;
    e=owner_persist(s); if(!e) *out=r; return e;
}
static int transition(struct owner_store *s,const struct owner_request *q,struct owner_receipt *r,unsigned op)
{
    if(op==OWNER_STATUS || op==OWNER_RESERVE) return 0;
    if(op==OWNER_ABORT) {
        if(r->state==OWNER_COMMITTED) return -EBUSY;
        if(r->state==OWNER_ABORTED) return 0;
        int intent=owner_intent(s,q); if(intent) return intent<0?intent:-EBUSY;
        r->state=OWNER_ABORTED; s->active=0; memset(s->pending,0,sizeof(s->pending));
        int e=owner_persist(s);
        if(!e) e=s->io.release(s->io.ctx,q,false);
        if(e) s->ready=false;
        return e;
    }
    if(r->state==OWNER_COMMITTED) return 0;
    if(r->state==OWNER_ABORTED || s->active!=r) return -EIO;
    if(op==OWNER_PREPARE && r->state==OWNER_PREPARED) return 0;
    if(op==OWNER_COMMIT && r->state!=OWNER_PREPARED) return -EIO;
    int e=s->io.reserve(s->io.ctx,q,s->pending,true);
    if(!e) e=owner_manifest_validate(s,q);
    if(e) { s->ready=false; return e; }
    if(op==OWNER_PREPARE) { r->state=OWNER_PREPARED; return owner_persist(s); }
    int intent=owner_intent(s,q); if(intent!=1) return intent<0?intent:-EIO;
    /* account COMMIT_REQUIRED forbids compensation even if publication or a
     * following flush fails. Pending identities stay denied until reopen. */
    e=s->io.publish(s->io.ctx,q,s->pending);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=owner_save_records(s,q,r);
    if(!e) e=s->io.release(s->io.ctx,q,true);
    if(e) s->ready=false;
    return e;
}
int owner_call(struct owner_store *s,const struct security_subject *subject,const void *wire,size_t n,void *reply,size_t cap,size_t *bytes)
{
    if(bytes) *bytes=0;
    if(!s || !wire || !reply || !bytes || n>OWNER_REQUEST_MAX || n<160) return -EINVAL;
    unsigned op=fs_rd32((const uint8_t *)wire+8);
    int e=security_subject_check(subject,(3u<<16)|op,0);
    const struct ciuki_credential *c=security_subject_credential(subject);
    if(e) return e;
    if(!c || c->uid) return -EPERM;
    if(!s->ready) return -EIO;
    if(!enter(s)) return -EBUSY;
    struct owner_request *q=oalloc(sizeof(*q)); if(!q) { leave(s); return -ENOMEM; }
    e=owner_request_decode(q,&op,wire,n);
    if(!e && (q->volume!=s->volume || memcmp(q->sid,s->sid,12))) e=-EACCES;
    if(!e && cap<96+32*q->count) e=-ENOSPC;
    struct owner_receipt *r=0; unsigned state=OWNER_UNKNOWN;
    if(!e && !q->count) e=owner_checkpoint_call(s,q,op,wire);
    else if(!e && q->transaction<=s->floor) { state=OWNER_CHECKPOINTED; if(op==OWNER_COMMIT) e=-EIO; }
    else if(!e) {
        for(unsigned i=0;i<s->receipt_count;i++) if(s->receipts[i]->transaction==q->transaction) { r=s->receipts[i]; break; }
        if(r && memcmp(r->body+128,q->digest,32)) e=-EIO;
        if(!e && !r && op==OWNER_RESERVE) e=reserve_batch(s,q,wire,n,&r);
        if(!e && !r && op!=OWNER_ABORT && op!=OWNER_STATUS) e=-EIO;
        if(!e && r) { e=transition(s,q,r,op); state=r->state; }
    }
    if(!q->count) state=OWNER_CHECKPOINTED;
    if(!e) e=owner_reply(q,r,state,reply,cap,bytes);
    ofree(q,sizeof(*q)); leave(s); return e;
}
int owner_recover(struct owner_store *s)
{
    if(!s || !s->ready) return -EIO;
    if(!s->active) return owner_checkpoint_recover(s);
    struct owner_request *q=oalloc(sizeof(*q)); if(!q) return -ENOMEM;
    unsigned op; struct owner_receipt *r=s->active;
    int e=owner_request_decode(q,&op,r->body,r->request_bytes);
    if(!e) e=s->io.reserve(s->io.ctx,q,s->pending,true);
    if(!e) e=owner_manifest_validate(s,q);
    int intent=e?e:owner_intent(s,q);
    if(!e && intent<0) e=intent;
    if(!e && intent==1) {
        if(r->state==OWNER_RESERVED) e=transition(s,q,r,OWNER_PREPARE);
        if(!e) e=transition(s,q,r,OWNER_COMMIT);
    } else if(!e) e=transition(s,q,r,OWNER_ABORT);
    ofree(q,sizeof(*q)); if(e) s->ready=false; return e;
}
int owner_open(struct owner_store *s,const struct owner_backend *io,const uint32_t sid[3],uint64_t volume,bool fresh)
{
    if(!s || !io || !sid || ozero(sid,12) || !volume || !io->read || !io->write || !io->flush || !io->barrier ||
       !io->manifest || !io->tree || !io->reserve || !io->publish || !io->release || !io->account_intent ||
       !io->account_current || !io->checkpoint_marker || !io->import_map || !io->occupied || !io->verify) return -EINVAL;
    memset(s,0,sizeof(*s)); s->io=*io; memcpy(s->sid,sid,12); s->volume=volume; s->uid_high=1000;
    int e=owner_load(s,fresh); if(!e) { s->ready=true; e=owner_recover(s); }
    for(unsigned i=0;!e && i<s->profile_count+s->root_count;i++) {
        const struct owner_record *r=i<s->profile_count?&s->profiles[i]:&s->roots[i-s->profile_count];
        if(!r->pending) e=s->io.verify(s->io.ctx,r);
    }
    if(e) s->ready=false; return e;
}
