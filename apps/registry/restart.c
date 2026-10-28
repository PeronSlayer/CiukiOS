/* Explicit little-endian journal, no persisted session id is authority.
 * SPDX-License-Identifier: MIT */
#include "restart.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
/* Header: magic8, version4, bytes4, SID12, scope4, uid4, reserved4,
 * volume8, journal generation8, next job id8, count4, reserved56, CRC4.
 * Record: id8,state4,kind4,mask4,phase4,uid4,origin session4,
 * account generation8,registry generation8,registry identity32,
 * string lengths5*u32,reserved12,stable proofs/hash80,strings<=832.
 * CRC covers the complete header and all records with CRC word zero. */
static uint32_t count(const struct rj_queue *q){return rb_get32(q->wire+64);}
static size_t size(const struct rj_queue *q){return RJ_HEADER+(size_t)count(q)*RJ_RECORD;}
static int isadmin(const struct rb_identity *c){if(!c->uid)return 1;for(unsigned i=0;i<c->group_count&&i<8;i++)if(c->groups[i]==544)return 1;return 0;}
static int seal(struct rj_queue *q)
{
    uint64_t generation=rb_get64(q->wire+48);if(generation==UINT64_MAX)return -EOVERFLOW;
    rb_put64(q->wire+48,generation+1);rb_put32(q->wire+12,(uint32_t)size(q));
    rb_put32(q->wire+124,0);rb_put32(q->wire+124,wr_crc(q->wire,size(q)));
    int e=q->backend.write(q->backend.context,rb_get32(q->wire+28),rb_get32(q->wire+32),q->wire,size(q));
    if(e)q->faulted=1;return e;
}
static int strings_valid(const uint8_t *job)
{
    size_t at=192;
    for(unsigned i=0;i<5;i++){
        uint32_t n=rb_get32(job+80+i*4);if(!n||n>RJ_RECORD-at||job[at+n-1]||memchr(job+at,0,n-1))return 0;
        if(i!=3&&n>260)return 0;
        at+=n;
    }
    for(;at<RJ_RECORD;at++)if(job[at])return 0;
    return 1;
}
static int valid(uint8_t *p,size_t n,unsigned scope,uint32_t uid,const uint32_t sid[3],uint64_t volume)
{
    if(n<RJ_HEADER||memcmp(p,"CIUKIRS2",8)||rb_get32(p+8)!=2||rb_get32(p+12)!=n||
       rb_get32(p+28)!=scope||rb_get32(p+32)!=uid||rb_get32(p+36)||rb_get64(p+40)!=volume||
       rb_get32(p+64)>RJ_JOBS||n!=RJ_HEADER+(size_t)rb_get32(p+64)*RJ_RECORD||!rb_get64(p+56))return 0;
    for(unsigned i=0;i<3;i++)if(rb_get32(p+16+i*4)!=sid[i])return 0;
    for(unsigned i=68;i<124;i++)if(p[i])return 0;
    uint32_t crc=rb_get32(p+124);rb_put32(p+124,0);uint32_t actual=wr_crc(p,n);rb_put32(p+124,crc);if(crc!=actual)return 0;
    uint64_t prior=0;
    for(unsigned i=0;i<rb_get32(p+64);i++){
        const uint8_t *j=p+RJ_HEADER+i*RJ_RECORD;uint64_t id=rb_get64(j);
        if(!id||id<=prior||id>=rb_get64(p+56)||rb_get32(j+8)<RJ_PREPARED||rb_get32(j+8)>RJ_CANCELLED||
           (rb_get32(j+12)!=RJ_FILE&&rb_get32(j+12)!=RJ_PROGRAM)||!rb_get32(j+16)||!rb_get32(j+28)||
           (scope==RJ_PERSONAL?rb_get32(j+24)!=uid:!rb_get32(j+24))||!strings_valid(j))return 0;
        for(unsigned r=100;r<112;r++)if(j[r])return 0;
        prior=id;
    }
    return 1;
}
int rj_open(struct rj_queue *q,const struct rb_authority *a,const struct rj_backend *b,unsigned scope,
            uint32_t uid,const uint32_t sid[3],uint64_t volume)
{
    if(!q||!a||!a->registry_role||!a->inspect||!a->retain||!a->release||!b||!b->read||!b->write||!b->account||
       !b->authorize||!b->apply||!b->launch||!sid||!volume||
       (scope==RJ_PERSONAL?!uid:uid!=0)||
       (scope!=RJ_PERSONAL&&scope!=RJ_MACHINE))return -EINVAL;
    int e=a->registry_role(a->context);if(e)return e;
    memset(q,0,sizeof(*q));q->authority=*a;q->backend=*b;
    uint8_t *data=NULL;size_t n=0;e=b->read(b->context,scope,uid,&data,&n);
    if(e&&e!=-ENOENT){free(data);return e;}
    if(!e&&!valid(data,n,scope,uid,sid,volume)){free(data);return -EIO;}
    q->wire=calloc(1,RJ_BYTES);if(!q->wire){free(data);return -ENOMEM;}
    if(!e)memcpy(q->wire,data,n);
    else{
        memcpy(q->wire,"CIUKIRS2",8);rb_put32(q->wire+8,2);rb_put32(q->wire+12,RJ_HEADER);
        for(unsigned i=0;i<3;i++)rb_put32(q->wire+16+4*i,sid[i]);
        rb_put32(q->wire+28,scope);rb_put32(q->wire+32,uid);rb_put64(q->wire+40,volume);rb_put64(q->wire+56,1);
    }
    free(data);return 0;
}
void rj_close(struct rj_queue *q){if(q->wire){memset(q->wire,0,RJ_BYTES);free(q->wire);}memset(q,0,sizeof(*q));}
static int identity(struct rj_queue *q,uintptr_t lease,struct rb_identity *c,int admission)
{
    if(!lease||!q->wire||q->faulted)return -EIO;
    int e=q->authority.inspect(q->authority.context,lease,c);if(e)return e;
    if(!c->session||!c->uid||(rb_get32(q->wire+28)==RJ_PERSONAL&&c->uid!=rb_get32(q->wire+32))||c->volume!=rb_get64(q->wire+40))return -EACCES;
    for(unsigned i=0;i<3;i++)if(c->machine_sid[i]!=rb_get32(q->wire+16+i*4))return -EACCES;
    struct rb_identity now={0};int enabled=0;e=q->backend.account(q->backend.context,c->uid,&now,&enabled);if(e)return e;
    if(!enabled)return -ENOENT;
    if(now.uid!=c->uid||now.group_count>8)return -EIO;
    if(rb_get32(q->wire+28)==RJ_MACHINE&&(!isadmin(&now)||!isadmin(c)))return -EACCES;
    if(admission&&c->role==7)return -EACCES; /* machine admission requires direct original client */
    c->account_generation=now.account_generation;return 0;
}
static int canonical(const char *s)
{
    if(!*s||!strcmp(s,"/"))return 1;if(*s!='/')return 0;
    for(const char *p=s;*p;){if(*p=='/'){p++;if(!*p)return 0;}const char *e=strchr(p,'/');size_t n=e?(size_t)(e-p):strlen(p);
        if(!n||(n==1&&*p=='.')||(n==2&&p[0]=='.'&&p[1]=='.')||memchr(p,'\\',n))return 0;
        for(size_t i=0;i<n;i++)if((unsigned char)p[i]<32)return 0;p+=n;}
    return 1;
}
int rj_admit(struct rj_queue *q,uintptr_t lease,const struct rj_spec *s,uint64_t *id)
{
    if(!q||!s||!id||!q->wire)return -EINVAL;if(q->busy)return -EAGAIN;int held=q->authority.retain(q->authority.context,lease);if(held)return held;q->busy=1;
    struct rb_identity c={0};int e=identity(q,lease,&c,rb_get32(q->wire+28)==RJ_MACHINE);if(e)goto done;
    if(count(q)==RJ_JOBS){e=-ENOSPC;goto done;}
    uint64_t next=rb_get64(q->wire+56);if(next==UINT64_MAX){e=-EOVERFLOW;goto done;}
    if((s->kind!=RJ_FILE&&s->kind!=RJ_PROGRAM)||!s->mask||
       (s->kind==RJ_PROGRAM&&(!s->executable||!*s->executable||!s->cwd||!*s->cwd))||
       (s->kind==RJ_FILE&&((!s->source||!*s->source)&&(!s->target||!*s->target)))){e=-EINVAL;goto done;}
    uint8_t job[RJ_RECORD]={0};rb_put64(job,next);rb_put32(job+8,RJ_PREPARED);rb_put32(job+12,s->kind);
    rb_put32(job+16,s->mask);rb_put32(job+20,s->phase);rb_put32(job+24,c.uid);rb_put32(job+28,c.session);
    rb_put64(job+32,c.account_generation);rb_put64(job+40,s->registry_generation);memcpy(job+48,s->registry_identity,32);
    const char *strings[]={s->source,s->target,s->executable,s->arguments,s->cwd};size_t at=192;
    for(unsigned i=0;i<5;i++){
        if(!strings[i]||(i!=3&&!canonical(strings[i]))){e=-EINVAL;goto done;}
        size_t n=strlen(strings[i])+1;if(n>RJ_RECORD-at||(i!=3&&n>260)){e=-E2BIG;goto done;}
        rb_put32(job+80+4*i,(uint32_t)n);memcpy(job+at,strings[i],n);at+=n;
    }
    e=q->backend.authorize(q->backend.context,lease,&c,rb_get32(q->wire+28),job,0);if(e)goto done;
    struct rb_identity fresh={0};e=identity(q,lease,&fresh,rb_get32(q->wire+28)==RJ_MACHINE);if(e)goto done;
    memcpy(q->wire+RJ_HEADER+(size_t)count(q)*RJ_RECORD,job,RJ_RECORD);
    rb_put32(q->wire+64,count(q)+1);rb_put64(q->wire+56,next+1);e=seal(q);if(!e)*id=next;
done:q->busy=0;q->authority.release(q->authority.context,lease);return e;
}
static uint8_t *find(struct rj_queue *q,uint64_t id){for(unsigned i=0;i<count(q);i++){uint8_t *j=q->wire+RJ_HEADER+i*RJ_RECORD;if(rb_get64(j)==id)return j;}return NULL;}
int rj_replay(struct rj_queue *q,uintptr_t lease,uint64_t id)
{
    if(!q||!q->wire)return -EINVAL;if(q->busy)return -EAGAIN;int held=q->authority.retain(q->authority.context,lease);if(held)return held;q->busy=1;
    struct rb_identity c={0};int e=identity(q,lease,&c,0);uint8_t *j=find(q,id);if(e)goto done;
    if(!j){e=-ENOENT;goto done;}
    if(rb_get32(j+24)!=c.uid){e=-EACCES;goto done;}
    unsigned state=rb_get32(j+8);if(state==RJ_COMPLETE||state==RJ_CANCELLED)goto done;
    if(state==RJ_HANDED){e=-EAGAIN;goto done;} /* uncertain launch, never duplicate */
    e=q->backend.authorize(q->backend.context,lease,&c,rb_get32(q->wire+28),j,1);if(e)goto done;
    if(rb_get32(j+12)==RJ_PROGRAM){
        rb_put32(j+8,RJ_HANDED);e=seal(q);if(e)goto done;
        e=identity(q,lease,&c,0);if(e)goto done;
        e=q->backend.launch(q->backend.context,lease,j);if(e)goto done;
    }else{
        if(state==RJ_PREPARED){rb_put32(j+8,RJ_APPLY_REQUIRED);e=seal(q);if(e)goto done;}
        if(state!=RJ_APPLIED){
            e=identity(q,lease,&c,0);if(e)goto done;
            e=q->backend.apply(q->backend.context,lease,j);if(e)goto done;
            rb_put32(j+8,RJ_APPLIED);e=seal(q);if(e)goto done;
        }
    }
    rb_put32(j+8,RJ_COMPLETE);e=seal(q);
done:q->busy=0;q->authority.release(q->authority.context,lease);return e;
}
int rj_reconcile(struct rj_queue *q,uintptr_t lease,uint64_t id,int completed)
{
    if(!q||!q->wire||(completed!=0&&completed!=1))return -EINVAL;if(q->busy)return -EAGAIN;int held=q->authority.retain(q->authority.context,lease);if(held)return held;q->busy=1;
    struct rb_identity c={0};int e=identity(q,lease,&c,0);uint8_t *j=find(q,id);
    if(!e&&(!j||rb_get32(j+8)!=RJ_HANDED))e=-EINVAL;
    if(!e&&rb_get32(j+24)!=c.uid)e=-EACCES;
    if(!e){rb_put32(j+8,completed?RJ_COMPLETE:RJ_CANCELLED);e=seal(q);}q->busy=0;q->authority.release(q->authority.context,lease);return e;
}
int rj_quarantine(struct rj_queue *q)
{
    if(!q||!q->backend.quarantine||q->faulted)return -EIO;
    return q->backend.quarantine(q->backend.context);
}

int rj_cancel_deleted(struct rj_queue *q)
{
    if(!q||!q->wire||q->faulted)return -EIO;if(q->busy)return -EAGAIN;
    int e=q->authority.registry_role(q->authority.context);if(e)return e;q->busy=1;int changed=0;
    for(unsigned i=0;i<count(q);i++){
        uint8_t *j=q->wire+RJ_HEADER+i*RJ_RECORD;unsigned state=rb_get32(j+8);
        if(state==RJ_COMPLETE||state==RJ_CANCELLED)continue;
        struct rb_identity c={0};int enabled=0;e=q->backend.account(q->backend.context,rb_get32(j+24),&c,&enabled);
        if(e&&e!=-ESRCH)break;
        if(e==-ESRCH||!enabled){rb_put32(j+8,RJ_CANCELLED);changed=1;e=0;}
    }
    if(!e&&changed)e=seal(q);q->busy=0;return e;
}
int rj_reapprove(struct rj_queue *q,uintptr_t lease,uint64_t old_id,uint64_t *new_id)
{
    if(!q||!q->wire||!new_id||rb_get32(q->wire+28)!=RJ_MACHINE)return -EINVAL;
    if(q->busy)return -EAGAIN;int held=q->authority.retain(q->authority.context,lease);if(held)return held;q->busy=1;
    struct rb_identity c={0};int e=identity(q,lease,&c,1);uint8_t *old=find(q,old_id);
    if(!e&&(!old||rb_get32(old+8)!=RJ_PREPARED))e=-EBUSY;
    if(!e&&count(q)==RJ_JOBS)e=-ENOSPC;
    uint64_t next=rb_get64(q->wire+56);if(!e&&next==UINT64_MAX)e=-EOVERFLOW;
    uint8_t job[RJ_RECORD];
    if(!e){memcpy(job,old,RJ_RECORD);rb_put64(job,next);rb_put32(job+24,c.uid);rb_put32(job+28,c.session);
        rb_put64(job+32,c.account_generation);memset(job+112,0,80);
        e=q->backend.authorize(q->backend.context,lease,&c,RJ_MACHINE,job,0);
        if(!e)e=identity(q,lease,&c,1);
        if(!e){memcpy(q->wire+RJ_HEADER+(size_t)count(q)*RJ_RECORD,job,RJ_RECORD);rb_put32(old+8,RJ_CANCELLED);
            rb_put32(q->wire+64,count(q)+1);rb_put64(q->wire+56,next+1);e=seal(q);if(!e)*new_id=next;}
    }
    q->busy=0;q->authority.release(q->authority.context,lease);return e;
}
