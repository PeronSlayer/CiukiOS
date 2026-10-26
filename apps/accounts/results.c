/* SPDX-License-Identifier: MIT -- ACCOUNT-only durable caller result journal */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>

#define RESULT_BYTES (128u + 96u * ACCT_RECEIPTS)
static int admin(const struct acct_subject *p)
{ for(unsigned i=0;p->uid && i<p->group_count;++i) if(p->groups[i]==544) return 1; return 0; }
static struct acct_receipt *find(struct acct_results *r, uint64_t id)
{ for(unsigned i=0;i<r->count;++i) if(r->receipts[i].id==id) return &r->receipts[i]; return NULL; }
static void public_receipt(const struct acct_receipt *r, uint64_t id, uint8_t p[80])
{
    memset(p,0,80); w32(p,1); w32(p+4,80); w32(p+8,r?r->state:ACCT_RECEIPT_EXPIRED); w64(p+32,id);
    if(!r) return;
    w32(p+12,r->operation); w32(p+16,(uint32_t)r->status); w32(p+20,r->requester); w32(p+24,r->target);
    w64(p+40,r->generation); memcpy(p+48,r->digest,32);
}
/* Independent permanent operation-id watermark. A missing/torn result snapshot
 * cannot turn an allocated id into an executable new request. */
static int watermark(struct acct_service *s, int write)
{
    uint64_t high=0,floor=0; int best=-1; size_t n;
    for(unsigned i=0;i<2;++i) {
        uint8_t p[96],sum[32]; const char *name=i?"accounts.ops.1":"accounts.ops.0";
        int e=s->store.io.read(s->store.io.ctx,name,p,sizeof(p),&n); if(e==-ENOENT) continue; if(e) return e;
        if(n!=96 || memcmp(p,"CIUKIOPS",8) || a64(p+8)!=s->store.volume || memcmp(p+16,s->db.sid,12) || a32(p+28) ||
            a64(p+40)>a64(p+32) || !azero(p+48,16)) return -EIO;
        sha256(p,64,sum); if(memcmp(sum,p+64,32)) return -EIO;
        if(a64(p+32)>high || (a64(p+32)==high && a64(p+40)>=floor)) { high=a64(p+32); floor=a64(p+40); best=(int)i; }
        else if(a64(p+40)>floor) return -EIO;
    }
    if(!write) {
        if(s->results->high>high) return -EIO;
        if(s->results->floor<floor) {
            unsigned remove=0;
            while(remove<s->results->count && s->results->receipts[remove].id<=floor) {
                if(s->results->receipts[remove].state==2) return -EIO;
                ++remove;
            }
            memmove(s->results->receipts,s->results->receipts+remove,(s->results->count-remove)*sizeof(*s->results->receipts));
            s->results->count-=remove; s->results->floor=floor;
        }
        s->results->high=high; return 0;
    }
    if(s->results->high<high || s->results->floor<floor) return -EIO;
    uint8_t p[96]={0}; memcpy(p,"CIUKIOPS",8); w64(p+8,s->store.volume); memcpy(p+16,s->db.sid,12);
    w64(p+32,s->results->high); w64(p+40,s->results->floor); sha256(p,64,p+64);
    const char *name=best==0?"accounts.ops.1":"accounts.ops.0";
    int e=s->store.io.write(s->store.io.ctx,name,p,96);
    if(!e) e=s->store.io.flush(s->store.io.ctx,name);
    if(!e) e=s->store.io.barrier(s->store.io.ctx);
    return e;
}
static int save(struct acct_service *s)
{
    struct acct_results *r=s->results; size_t n=128+96*r->count;
    uint8_t *p=s->store.kernel.allocate(s->store.kernel.ctx,n); if(!p) return -ENOMEM;
    memset(p,0,n); memcpy(p,"CIUKIRSL",8); w32(p+8,1); w32(p+12,r->count);
    w64(p+16,r->high); w64(p+24,r->floor); w64(p+32,s->db.generation);
    w64(p+40,s->store.high_generation); w32(p+48,s->store.high_uid); memcpy(p+52,s->db.sid,12);
    uint8_t db[ACCT_FILE_MAX]; size_t bytes;
    int e=acct_encode(&s->db,0,db,sizeof(db),&bytes);
    if(!e) sha256(db,bytes,p+64);
    for(unsigned i=0;i<r->count;++i) {
        public_receipt(&r->receipts[i],r->receipts[i].id,p+128+96*i);
        w64(p+128+96*i+80,r->receipts[i].planned_generation);
    }
    if(!e) e=acct_blob_write(&s->store,"accounts.results",p,n,&r->sequence);
    acct_wipe(db,sizeof(db)); acct_wipe(p,n); s->store.kernel.deallocate(s->store.kernel.ctx,p,n); return e;
}
static void compact(struct acct_results *r)
{
    if(r->count<768) return;
    unsigned remove=0;
    while(remove<r->count-256 && r->receipts[remove].state!=ACCT_RECEIPT_PENDING) ++remove;
    if(remove) {
        r->floor=r->receipts[remove-1].id;
        memmove(r->receipts,r->receipts+remove,(r->count-remove)*sizeof(*r->receipts));
        r->count-=remove; memset(r->receipts+r->count,0,remove*sizeof(*r->receipts));
    }
}
int acct_results_open(struct acct_service *s)
{
    struct acct_kernel *k=&s->store.kernel;
    s->results=k->allocate(k->ctx,sizeof(*s->results)); if(!s->results) return -ENOMEM;
    memset(s->results,0,sizeof(*s->results));
    uint8_t *p=k->allocate(k->ctx,RESULT_BYTES); if(!p) return -ENOMEM;
    size_t n=0; uint64_t sequence=0;
    int e=acct_blob_read(&s->store,"accounts.results",p,RESULT_BYTES,&n,&sequence);
    if(e==-ENOENT) e=0;
    else if(!e) {
        unsigned count=n>=128?a32(p+12):0;
        if(n<128 || memcmp(p,"CIUKIRSL",8) || a32(p+8)!=1 || count>1024 || n!=128+96*count ||
            a64(p+24)>a64(p+16) || a64(p+32)>s->db.generation || a64(p+40)>s->store.high_generation ||
            a32(p+48)>s->store.high_uid || memcmp(p+52,s->db.sid,12) || !azero(p+96,32)) e=-EIO;
        if(!e && a64(p+32)==s->db.generation) {
            uint8_t db[ACCT_FILE_MAX], digest[32]; size_t bytes;
            e=acct_encode(&s->db,0,db,sizeof(db),&bytes); if(!e) { sha256(db,bytes,digest); if(memcmp(digest,p+64,32)) e=-EIO; }
            acct_wipe(db,sizeof(db));
        }
        if(!e) {
            struct acct_results *r=s->results; r->high=a64(p+16); r->floor=a64(p+24); r->sequence=sequence; r->count=count;
            uint64_t prior=r->floor; unsigned pending=0,settled=0;
            for(unsigned i=0;!e && i<count;++i) {
                const uint8_t *b=p+128+96*i; struct acct_receipt *x=&r->receipts[i];
                x->state=a32(b+8); x->operation=a32(b+12); x->status=(int32_t)a32(b+16); x->requester=a32(b+20);
                x->target=a32(b+24); x->id=a64(b+32); x->generation=a64(b+40); memcpy(x->digest,b+48,32); x->planned_generation=a64(b+80);
                if(a32(b)!=1 || a32(b+4)!=80 || a32(b+28) || a64(b+88) || x->id<=prior || x->id>r->high ||
                    x->state<1 || x->state>4 || x->operation<2 || x->operation>5 || !x->requester ||
                    (x->state<=2 && (x->status || x->generation)) ||
                    (x->state==3 && (x->status || !x->generation || x->generation>s->db.generation)) ||
                    (x->state==4 && (x->status>=0 || x->generation))) { e=-EIO; break; }
                prior=x->id;
                if(x->state==ACCT_RECEIPT_RESERVED) { x->state=4; x->status=-ECANCELED; ++settled; }
                else if(x->state==ACCT_RECEIPT_PENDING) {
                    if(++pending>1 || !x->planned_generation || x->planned_generation<a64(p+32)) { e=-EIO; break; }
                    if(s->db.generation==x->planned_generation) { x->state=3; x->generation=s->db.generation; }
                    else if(s->db.generation<x->planned_generation) { x->state=4; x->status=-ECANCELED; }
                    else e=-EIO;
                }
            }
            if(!e && (pending || settled)) e=save(s);
        }
    }
    if(!e) e=watermark(s,0);
    acct_wipe(p,RESULT_BYTES); k->deallocate(k->ctx,p,RESULT_BYTES); return e;
}
int acct_results_execute(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    struct acct_request *q=&job->request; struct acct_results *r=s->results;
    if(!r) return -EIO;
    if(q->operation==ACCT_STATUS) {
        if(!q->client_operation_id || q->client_operation_id>r->high) return -ESRCH;
        struct acct_receipt *x=find(r,q->client_operation_id);
        if(x && x->requester!=job->subject.uid) return -EACCES;
        public_receipt(x,q->client_operation_id,out->body); out->body_bytes=80; return 0;
    }
    unsigned op=q->flags;
    if(op<2 || op>5 || !job->subject.uid || (op!=2 && op!=4 && !admin(&job->subject))) return -EPERM;
    if(azero(q->request_digest,32)) return -EINVAL;
    for(unsigned i=0;i<r->count;++i) {
        struct acct_receipt *x=&r->receipts[i];
        if(x->requester==job->subject.uid && x->state==ACCT_RECEIPT_PENDING) return -EAGAIN;
    }
    /* Retire any unsubmitted predecessor without disclosing or reusing it. */
    for(unsigned i=0;i<r->count;++i) if(r->receipts[i].requester==job->subject.uid && r->receipts[i].state==1) {
        memmove(r->receipts+i,r->receipts+i+1,(r->count-i-1)*sizeof(*r->receipts)); --r->count; --i;
    }
    compact(r);
    if(r->count==1024) return -EAGAIN;
    if(r->high==UINT64_MAX) return -EOVERFLOW;
    struct acct_receipt *x=&r->receipts[r->count++]; memset(x,0,sizeof(*x));
    x->id=++r->high; x->requester=job->subject.uid; x->operation=op; x->state=1; memcpy(x->digest,q->request_digest,32);
    int e=watermark(s,1);
    if(!e) e=save(s); if(e) { s->available=0; s->faulted=1; return e; }
    w64(out->body,x->id); w64(out->body+8,s->db.generation); out->body_bytes=16; return 0;
}
int acct_results_admit(struct acct_service *s, struct acct_job *job, struct acct_response *out, int *cached)
{
    *cached=0; struct acct_request *q=&job->request;
    if(!q->client_operation_id) return -EINVAL;
    struct acct_receipt *x=find(s->results,q->client_operation_id);
    if(!x) return q->client_operation_id<=s->results->high?-EAGAIN:-ESRCH;
    if(x->requester!=job->subject.uid) return -EACCES;
    uint8_t digest[32]; int e=acct_management_digest(q,digest);
    if(e) return e;
    if(x->operation!=q->wire_operation || memcmp(x->digest,digest,32)) return -EINVAL;
    if(x->state==3 || x->state==4) { *cached=1; out->generation=x->generation; return x->status; }
    if(x->state==2) return -EAGAIN;
    if(q->generation!=s->db.generation) return -EAGAIN;
    if(s->store.high_generation==UINT64_MAX) return -EOVERFLOW;
    x->state=2; x->target=q->uid; x->planned_generation=s->store.high_generation+1;
    e=save(s); if(e) { s->available=0; s->faulted=1; } return e;
}
int acct_results_replan(struct acct_service *s, const struct acct_request *q)
{
    struct acct_receipt *x=find(s->results,q->client_operation_id);
    if(!x || x->state!=ACCT_RECEIPT_PENDING) return -EIO;
    if(s->store.high_generation==UINT64_MAX) return -EOVERFLOW;
    x->planned_generation=s->store.high_generation+1;
    int e=save(s); if(e) { s->available=0; s->faulted=1; } return e;
}
int acct_results_finish(struct acct_service *s, const struct acct_request *q, int result)
{
    struct acct_receipt *x=find(s->results,q->client_operation_id); if(!x || x->state!=2) return -EIO;
    if(result && !s->available) return result; /* recovery settles the authorized intent */
    x->state=result?4:3; x->status=result; x->generation=result?0:s->db.generation;
    int e=save(s); if(e) { s->available=0; s->faulted=1; } return e?e:result;
}

int acct_results_cancel(struct acct_service *s, const struct acct_job *job)
{
    if(!s->results || !job->request.client_operation_id) return 0;
    struct acct_receipt *x=find(s->results,job->request.client_operation_id);
    if(!x || x->state!=1 || x->requester!=job->subject.uid) return 0;
    uint8_t digest[32]; int e=acct_management_digest(&job->request,digest);
    if(e || memcmp(digest,x->digest,32) || x->operation!=job->request.wire_operation) return 0;
    x->state=4; x->status=-ECANCELED;
    e=save(s); if(e) { s->available=0; s->faulted=1; } return e;
}
