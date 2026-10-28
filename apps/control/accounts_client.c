/* SPDX-License-Identifier: MIT
 * sec-user-04: User Accounts client of the SYSTEM account service; see
 * accounts_client.h. Wire layouts follow apps/accounts/protocol.c (the
 * service's decoder is the authority; this file only encodes them).
 */
#include "accounts_client.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <string.h>

#define OP_LOOKUP 1u
#define OP_PASSWORD 2u
#define OP_CREATE 3u
#define OP_UPDATE 4u
#define OP_DELETE 5u
#define OP_LIST 6u
#define OP_STATUS 7u
#define OP_RESERVE 9u
#define SEL_ENABLE 256u
#define SEL_ROLE 512u
#define SEL_NAME 1024u
#define SEL_PASSWORD 2048u
#define SEL_PICTURE 4096u
#define RECEIPT_PENDING 2u
#define RECEIPT_COMMITTED 3u
#define RECEIPT_ABORTED 4u
#define STATUS_TRIES 20u
#define LIST_TRIES 8u        /* fx-uaccounts: busy LIST, about 2 s in all */
#define LIST_PAUSE_MS 250u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p)|(uint64_t)rd32(p+4)<<32; }
static void wr32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p,(uint32_t)v); wr32(p+4,(uint32_t)(v>>32)); }
static size_t bounded(const char *s, size_t cap) { const char *e=memchr(s,0,cap); return e ? (size_t)(e-s) : cap; }

int cpa_management_encode(uint32_t wire, uint32_t flags, uint32_t uid, uint64_t generation, const char *name,
                          const char *old_password, const char *password, const char *picture,
                          uint64_t operation_id, uint8_t p[312], uint8_t digest[32])
{
    size_t nn=bounded(name,CPA_NAME), no=bounded(old_password,CPA_SECRET), np=bounded(password,CPA_SECRET),
           nq=bounded(picture,CPA_PICTURE_MAX);
    if (nn>=CPA_NAME || no>=CPA_SECRET || np>=CPA_SECRET || nq>=CPA_PICTURE_MAX) return -EINVAL;
    memset(p,0,312);
    wr32(p,1); wr32(p+4,flags); wr32(p+24,(uint32_t)nn); wr32(p+28,(uint32_t)no); wr32(p+32,(uint32_t)np);
    wr32(p+36,uid); wr64(p+40,generation);
    memcpy(p+48,name,nn); memcpy(p+80,old_password,no); memcpy(p+144,password,np); memcpy(p+208,picture,nq);
    wr64(p+304,operation_id);
    if (digest) {
        /* acct_management_digest: operation||management without the id. */
        uint8_t d[316]; memset(d,0,sizeof(d));
        wr32(d,wire); wr32(d+4,1); wr32(d+8,flags); wr32(d+28,(uint32_t)nn); wr32(d+32,(uint32_t)no);
        wr32(d+36,(uint32_t)np); wr32(d+40,uid); wr64(d+44,generation);
        memcpy(d+52,name,nn); memcpy(d+84,old_password,no); memcpy(d+148,password,np); memcpy(d+212,picture,nq);
        sha256(d,sizeof(d),digest);
        cpa_wipe(d,sizeof(d));
    }
    return 0;
}
static int list(const struct cpa_transport *t, struct cpa_view *v)
{
    uint8_t reply[16+144*CPA_MAX]; size_t used=0; int status=0;
    /* LIST is read-only: a busy service is asked again (bounded), so the
     * page does not report "not available" for a transient refusal. */
    for (unsigned attempt=1;;attempt++) {
        int e=t->call(t->ctx,OP_LIST,0,NULL,0,reply,sizeof(reply),&used,&status);
        if (e) return e;
        if (status!=-EAGAIN || !t->pause || attempt>=LIST_TRIES) break;
        t->pause(t->ctx,LIST_PAUSE_MS);
    }
    if (status) return status;
    if (used<16 || rd32(reply)!=1 || rd32(reply+4)>CPA_MAX || used!=16+144*(size_t)rd32(reply+4)) return -EIO;
    v->generation=rd64(reply+8); v->count=rd32(reply+4);
    for (unsigned i=0;i<v->count;i++) {
        const uint8_t *b=reply+16+144*i; struct cpa_record *r=&v->records[i];
        r->uid=rd32(b); r->groups=rd32(b+4); r->flags=rd32(b+8)&3u;
        memcpy(r->name,b+16,CPA_NAME); r->name[CPA_NAME-1]=0;
        memcpy(r->picture,b+48,CPA_PICTURE_MAX); r->picture[CPA_PICTURE_MAX-1]=0;
    }
    v->valid=1;
    return 0;
}
static int identity(const struct cpa_transport *t, struct cpa_view *v)
{
    uint32_t uid=0, groups[8]={0}; unsigned count=0;
    int e=t->self ? t->self(t->ctx,&uid,groups,&count) : -ENOSYS;
    if (e) return e;
    v->self_uid=uid; v->self_admin=v->self_guest=0;
    for (unsigned i=0;i<count && i<8;i++) {
        if (groups[i]==CPA_GROUP_ADMINS && uid) v->self_admin=1;
        if (groups[i]==CPA_GROUP_GUESTS) v->self_guest=1;
    }
    return 0;
}
/* A lost management reply: the durable receipt decides (A2-4). Never
 * repeats the operation and never infers rollback from silence. */
static int settle(const struct cpa_transport *t, uint64_t id)
{
    for (unsigned attempt=0;attempt<STATUS_TRIES;attempt++) {
        uint8_t body[32], reply[80]; size_t used=0; int status=0;
        memset(body,0,sizeof(body)); wr32(body,1); wr64(body+8,id);
        int e=t->call(t->ctx,OP_STATUS,0,body,sizeof(body),reply,sizeof(reply),&used,&status);
        if (e) continue;
        if (status) return status==-ESRCH ? -EIO : status;
        if (used!=80 || rd32(reply)!=1 || rd64(reply+32)!=id) return -EIO;
        uint32_t state=rd32(reply+8);
        if (state==RECEIPT_COMMITTED) return 0;
        if (state==RECEIPT_ABORTED) { int32_t s=(int32_t)rd32(reply+16); return s<0 ? s : -EIO; }
        if (state==1u) return -ENOTCONN;   /* RESERVED: the change never reached the service */
        if (state!=RECEIPT_PENDING) return -EIO;   /* EXPIRED: unknown, never a rollback */
    }
    return -EIO;
}
static int mutate(const struct cpa_transport *t, const struct cpa_request *q, const struct cpa_view *v)
{
    const struct cpa_record *r=q->action==CPA_CREATE ? NULL : cpa_find(v,q->uid);
    if (q->action!=CPA_CREATE && !r) return -ENOENT;
    if (!cpa_allowed(v,q->action,r)) return -EPERM;   /* the service re-checks at apply time */
    uint32_t wire=OP_UPDATE, flags=0, uid=q->uid;
    const char *name="", *old="", *password="", *picture="";
    switch (q->action) {
    case CPA_CREATE:
        wire=OP_CREATE; uid=0; flags=(q->enable ? 1u : 0u)|(q->admin ? 2u : 0u); name=q->name; password=q->password; break;
    case CPA_DELETE: wire=OP_DELETE; break;
    case CPA_RENAME: flags=SEL_NAME; name=q->name; break;
    case CPA_TYPE: flags=SEL_ROLE|(q->admin ? 2u : 0u); break;
    case CPA_ENABLE: flags=SEL_ENABLE|(q->enable ? 1u : 0u); break;
    case CPA_PICTURE: flags=SEL_PICTURE; picture=q->picture; break;
    case CPA_PASSWORD:
        /* Own password: PASSWORD with the old one (Guest too, contract 7.1);
         * an administrator resets another account through UPDATE. */
        if (r->uid==v->self_uid) { wire=OP_PASSWORD; old=q->old_password; }
        else flags=SEL_PASSWORD;
        password=q->password; break;
    default: return -EINVAL;
    }
    if (q->action==CPA_CREATE || q->action==CPA_PASSWORD) {
        size_t n=bounded(q->password,CPA_SECRET);
        if (n<8 || n>63 || strncmp(q->password,q->confirm,CPA_SECRET)) return -EINVAL;
    }
    if ((q->action==CPA_CREATE || q->action==CPA_RENAME) && (!q->name[0] || bounded(q->name,CPA_NAME)>=CPA_NAME)) return -EINVAL;
    uint8_t body[312], digest[32], reserve[48], reply[16]; size_t used=0; int status=0;
    int e=cpa_management_encode(wire,flags,uid,v->generation,name,old,password,picture,0,body,digest);
    if (!e) {
        memset(reserve,0,sizeof(reserve)); wr32(reserve,1); wr32(reserve+4,wire); memcpy(reserve+8,digest,32);
        e=t->call(t->ctx,OP_RESERVE,0,reserve,sizeof(reserve),reply,sizeof(reply),&used,&status);
        if (!e) e=status;
        if (!e && used!=16) e=-EIO;
    }
    if (!e) {
        uint64_t id=rd64(reply);
        if (!id) e=-EIO;
        else {
            wr64(body+304,id);
            e=t->call(t->ctx,wire,v->generation,body,sizeof(body),reply,sizeof(reply),&used,&status);
            if (e<0) e=settle(t,id);   /* reply lost: the durable receipt decides */
            else if (!e) e=status;
        }
    }
    cpa_wipe(body,sizeof(body)); cpa_wipe(digest,sizeof(digest)); cpa_wipe(reserve,sizeof(reserve));
    return e;
}
int cpa_execute(const struct cpa_transport *t, struct cpa_request *q, struct cpa_view *v)
{
    int e=t && q && v && t->call ? 0 : -EINVAL;
    if (!e) {
        struct cpa_view next; memset(&next,0,sizeof(next));
        e=identity(t,&next);
        if (!e) e=list(t,&next);
        if (!e) *v=next;
    }
    if (!e && q->action!=CPA_READ) {
        e=mutate(t,q,v);
        struct cpa_view next=*v;
        if (!list(t,&next)) *v=next;   /* the applied (or refused) state, fresh */
    }
    if (q) {
        cpa_wipe(q->old_password,sizeof(q->old_password)); cpa_wipe(q->password,sizeof(q->password));
        cpa_wipe(q->confirm,sizeof(q->confirm));
    }
    return e;
}
