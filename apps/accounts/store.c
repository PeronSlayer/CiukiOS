/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>

#define DB "accounts.db"
#define NEW "accounts.db.new"
#define BAK "accounts.db.bak"
#define TXN "accounts.db.txn"
#define INTENT_MAX (192u + ACCT_OWNER_BODY_MAX + ACCT_FILE_MAX)
struct workspace {
    uint8_t intent[INTENT_MAX], bytes[ACCT_FILE_MAX];
    uint8_t current_bytes[ACCT_FILE_MAX];
    size_t intent_bytes, db_bytes;
    struct acct_owner_request q;
    struct acct_owner_reply r;
    struct acct_db next;
    struct acct_db current;
    uint8_t manifest[ACCT_MANIFEST_MAX];
    uint8_t seed[ACCT_FILE_MAX], enrollment[1464], expected[ACCT_FILE_MAX];
    struct acct_db recipe;
};
static int rm(struct acct_store *s, const char *p)
{ int e=s->io.remove(s->io.ctx,p); return e==-ENOENT?0:e; }
static int durable(struct acct_store *s, const char *p, const void *v, size_t n)
{
    int e=s->io.write(s->io.ctx,p,v,n);
    if(!e) e=s->io.flush(s->io.ctx,p);
    if(!e) e=s->io.barrier(s->io.ctx);
    return e;
}
static struct workspace *alloc(struct acct_store *s)
{
    struct workspace *w=s->kernel.allocate(s->kernel.ctx,sizeof(*w));
    if(w) memset(w,0,sizeof(*w)); return w;
}
static int finish(struct acct_store *s, struct workspace *w, int e)
{ acct_wipe(w,sizeof(*w)); s->kernel.deallocate(s->kernel.ctx,w,sizeof(*w)); return e; }
static int read_db(struct acct_store *s, const char *p, uint8_t *bytes, struct acct_db *d, size_t *n)
{
    int e=s->io.read(s->io.ctx,p,bytes,ACCT_FILE_MAX,n);
    return e?e:acct_decode(d,0,bytes,*n);
}
/* Two permanent allocation slots. Never overwrite the newest slot. The digest
 * seals volume, SID, uid and G watermarks independently of published accounts.
 * A torn/corrupt slot fails closed; an old backup cannot lower reservations. */
static int watermarks(struct acct_store *s, const uint32_t sid[3], int reserve, uint64_t g, uint32_t uid)
{
    uint64_t high=0; uint32_t next=1001; int best=-1; size_t n;
    for(unsigned i=0;i<2;++i) {
        uint8_t p[97], sum[32]; const char *name=i?"accounts.alloc.1":"accounts.alloc.0";
        int e=s->io.read(s->io.ctx,name,p,sizeof(p),&n); if(e==-ENOENT) continue;
        if(e || n!=96 || memcmp(p,"CIUKIALC",8) || a64(p+8)!=s->volume ||
            !azero(p+40,24)) return -EIO;
        for(unsigned j=0;j<3;++j) if(a32(p+16+4*j)!=sid[j]) return -EIO;
        sha256(p,64,sum); if(!acct_equal_secret(sum,p+64,32) || !a64(p+32) || a32(p+28)<1001) return -EIO;
        if(a64(p+32)>high) {
            if(a32(p+28)<next) return -EIO;
            high=a64(p+32); next=a32(p+28); best=(int)i;
        } else if(a32(p+28)>next || (a64(p+32)==high && a32(p+28)!=next)) return -EIO;
    }
    s->high_generation=high; s->high_uid=next;
    if(!reserve) return 0;
    if(high==UINT64_MAX) return -EOVERFLOW;
    if(g!=high+1 || uid<next) return -EINVAL;
    uint8_t p[96]={0}; memcpy(p,"CIUKIALC",8); w64(p+8,s->volume);
    for(unsigned j=0;j<3;++j) w32(p+16+4*j,sid[j]);
    w32(p+28,uid); w64(p+32,g); sha256(p,64,p+64);
    int e=durable(s,best==0?"accounts.alloc.1":"accounts.alloc.0",p,96);
    if(!e) { s->high_generation=g; s->high_uid=uid; }
    return e;
}
static int bindings(struct acct_store *s, const struct acct_db *d)
{
    for(unsigned i=0;i<d->count;++i) {
        char root[64]={0}; struct acct_owner_binding b={0};
        int e=s->owner.binding(s->owner.ctx,s->volume,d->records[i].uid,root,&b);
        /* ENOENT here is a missing committed binding, never the unpublished
         * store sentinel consumed by service open. Only EAGAIN is transient. */
        if(e) return acct_boot_result(s,ACCT_BOOT_BINDING,e==-EAGAIN?e:-EIO);
        if(!b.key || !b.generation || b.volume!=s->volume ||
            b.uid!=d->records[i].uid || b.action==ACCT_OWNER_RETIRE || !acct_profile_root_valid(root)) return -EIO;
    }
    return 0;
}
int acct_owner_requests(struct acct_store *s, const struct acct_db *d, struct acct_owner_request *q, unsigned *count)
{
    if(!s || !d || !q || !count || d->count<2 || d->count>ACCT_MAX) return -EINVAL;
    memset(q,0,sizeof(*q)); q->volume=s->volume; q->transaction=d->generation;
    memcpy(q->sid,d->sid,12); int e=acct_boot_result(s,ACCT_BOOT_GENERATION,s->owner.generation(s->owner.ctx,&q->expected_generation));
    for(unsigned i=0;!e && i<d->count;++i) {
        struct acct_owner_binding *b=&q->bindings[q->count++]; b->uid=d->records[i].uid; b->volume=s->volume;
        e=s->owner.binding(s->owner.ctx,s->volume,b->uid,b->root,b);
        if(e==-ENOENT) {
            memset(b,0,sizeof(*b)); b->uid=d->records[i].uid; b->volume=s->volume; b->action=ACCT_OWNER_CREATE;
            snprintf(b->root,64,"/Windows/Profiles/u%08x",b->uid); e=0;
        } else if(!e) { b->action=ACCT_OWNER_KEEP; b->locator=0; }
        if(e) acct_boot_result(s,ACCT_BOOT_BINDING,e);
    }
    *count=e?0:1; return e;
}
static int marker(struct acct_store *s, struct workspace *w, const char *name, int create)
{
    uint8_t p[96]={0}, found[97]; size_t n;
    memcpy(p,"CIUKIPHS",8); w64(p+8,w->q.transaction); memcpy(p+16,w->q.digest,32);
    /* The filename is part of the checksum; a phase cannot be relabelled. */
    memcpy(p+48,name,strlen(name)); sha256(p,64,p+64);
    int e=s->io.read(s->io.ctx,name,found,sizeof(found),&n);
    if(e==-ENOENT && create) return durable(s,name,p,96);
    if(e) return e;
    return n==96 && acct_equal_secret(p,found,96)?0:-EIO;
}
static int sid_copy(struct acct_store *s, const struct acct_db *d, int create)
{
    uint8_t p[64]={0}, found[65]; size_t n;
    memcpy(p,"CIUKISID",8); for(unsigned i=0;i<3;++i) w32(p+8+4*i,d->sid[i]);
    w64(p+24,s->volume); sha256(p,32,p+32);
    int e=s->io.read(s->io.ctx,"machine.sid",found,sizeof(found),&n);
    if(e==-ENOENT && create) return durable(s,"machine.sid",p,64);
    if(e) return e;
    return n==64 && acct_equal_secret(p,found,64)?0:-EIO;
}
/* A matching SID intent survives interruption before the full verifier/output
 * is sealed. Once written, it is reused; entropy is never requested for another
 * machine identity. No blank verifier is derived before machine.sid is flushed. */
int acct_boot_identity(struct acct_store *s, struct acct_db *d, int create)
{
    struct acct_bootstrap b; int e=acct_boot_read(s,&b); if(e) return e;
    if(b.state!=1 || b.origin!=d->origin) return -EIO;
    uint8_t p[129], sum[32]; size_t n;
    e=s->io.read(s->io.ctx,"bootstrap.sid",p,sizeof(p),&n);
    if(e==-ENOENT) {
        uint8_t existing[65]; size_t m;
        if(s->io.read(s->io.ctx,"machine.sid",existing,sizeof(existing),&m)!=-ENOENT) return -EIO;
        if(!create) return -ENOENT;
        memset(p,0,128); memcpy(p,"CIUKISPR",8); w32(p+8,b.origin); w64(p+16,s->volume); memcpy(p+40,b.seed_digest,32);
        e=acct_boot_result(s,ACCT_BOOT_ENTROPY_SID,s->kernel.entropy(s->kernel.ctx,p+24,12)); if(e) return e;
        if(azero(p+24,12)) return acct_boot_result(s,ACCT_BOOT_ENTROPY_SID,-EAGAIN);
        sha256(p,96,p+96); e=durable(s,"bootstrap.sid",p,128); if(e) return e;
    } else {
        if(e || n!=128 || memcmp(p,"CIUKISPR",8) || a32(p+8)!=b.origin || a32(p+12) ||
            a64(p+16)!=s->volume || azero(p+24,12) || a32(p+36) || memcmp(p+40,b.seed_digest,32) || !azero(p+72,24)) return -EIO;
        sha256(p,96,sum); if(!acct_equal_secret(sum,p+96,32)) return -EIO;
    }
    for(unsigned i=0;i<3;++i) d->sid[i]=a32(p+24+4*i);
    e=sid_copy(s,d,create);
    return !create && e==-ENOENT?0:e;
}
static int advance_boot(struct acct_store *s, struct workspace *w, unsigned state)
{
    struct acct_bootstrap b; int e=acct_boot_read(s,&b); if(e) return e;
    if(b.origin!=w->next.origin || (b.state>1 && memcmp(b.sid,w->next.sid,12))) return -EIO;
    if(b.state==ACCT_UNPROVISIONED) {
        if(w->q.expected_accounts_generation) return -EIO;
        /* The immutable reservation seals the full output including fresh blank
         * verifier before SID_READY. machine.sid is durable first. */
        e=sid_copy(s,&w->next,1); if(e) return e;
        memcpy(b.sid,w->next.sid,12); b.transaction=w->q.transaction; b.state=ACCT_SID_READY;
        b.next_uid=w->next.next_uid; e=acct_boot_write(s,&b); if(e) return e;
    }
    if(sid_copy(s,&w->next,0)) return -EIO;
    if(!w->q.expected_accounts_generation && b.transaction!=w->q.transaction) return -EIO;
    while(b.state<state) {
        ++b.state;
        if(b.state==ACCT_OWNERS_READY) { b.owner_generation=w->r.generation; b.flags|=2; }
        if(b.state==ACCT_ACCOUNTS_READY) b.accounts_generation=w->q.transaction;
        if(b.state==ACCT_CONSUMED) b.flags|=1;
        e=acct_boot_write(s,&b); if(e) return e;
    }
    if(state==ACCT_CONSUMED && (b.accounts_generation!=w->next.generation ||
        !!(b.flags&4)!=!!(w->next.flags&4) || b.owner_generation!=w->r.generation || b.next_uid<w->next.next_uid)) {
        if((b.flags&4) && !(w->next.flags&4)) return -EIO;
        b.transaction=w->next.generation; b.accounts_generation=w->next.generation;
        b.owner_generation=w->r.generation; b.flags|=w->next.flags&4;
        b.next_uid=w->next.next_uid; e=acct_boot_write(s,&b);
    }
    return e;
}
static int cleanup(struct acct_store *s, const struct workspace *w)
{
    static const char *const names[]={BAK,NEW,"acct.prepared","acct.commit","acct.owner","acct.done","acct.abort"};
    int e=0; for(unsigned i=0;!e && i<sizeof(names)/sizeof(*names);++i) e=rm(s,names[i]);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=rm(s,TXN); if(!e) e=s->io.barrier(s->io.ctx);
    char path[128]; snprintf(path,sizeof(path),"/System/Security/Staging/g%016llx/manifest",(unsigned long long)w->q.transaction);
    if(!e) e=rm(s,path); if(!e) e=s->io.barrier(s->io.ctx); return e;
}
static int decode_intent(struct acct_store *s, struct workspace *w)
{
    uint8_t *p=w->intent; unsigned op;
    if(w->intent_bytes<192 || memcmp(p,"CIACTXN3",8) || a32(p+8)!=w->intent_bytes ||
        a32(p+12)>ACCT_OWNER_BODY_MAX || a32(p+16)>ACCT_FILE_MAX || !azero(p+20,12) ||
        w->intent_bytes!=192+a32(p+12)+a32(p+16) || !azero(p+128,64)) return -EIO;
    uint8_t sum[32], saved[32]; memcpy(saved,p+96,32); memset(p+96,0,32); sha256(p,w->intent_bytes,sum); memcpy(p+96,saved,32);
    if(!acct_equal_secret(sum,saved,32) || acct_owner_decode(&w->q,&op,p+192,a32(p+12)) || op!=ACCT_OWNER_PREPARE ||
        w->q.volume!=s->volume || acct_decode(&w->next,0,p+192+a32(p+12),a32(p+16)) ||
        w->next.generation!=w->q.transaction || memcmp(w->next.sid,w->q.sid,12)) return -EIO;
    w->db_bytes=a32(p+16); memcpy(w->bytes,p+192+a32(p+12),w->db_bytes);
    sha256(w->bytes,w->db_bytes,sum);
    return acct_equal_secret(sum,w->q.target_digest,32)?0:-EIO;
}
static int run_intent(struct acct_store *s, struct workspace *w)
{
    int e=watermarks(s,w->next.sid,0,0,0);
    if(e || s->high_generation<w->next.generation || s->high_uid<w->next.next_uid) return -EIO;
    if(!w->q.expected_accounts_generation) { e=advance_boot(s,w,ACCT_SID_READY); if(e) return e; }
    e=acct_owner_call(s,ACCT_OWNER_STATUS,&w->q,&w->r); if(e) return e;
    int irrevocable=marker(s,w,"acct.commit",0);
    if(irrevocable && irrevocable!=-ENOENT) return irrevocable;
    int abort_marker=marker(s,w,"acct.abort",0);
    if(abort_marker && abort_marker!=-ENOENT) return abort_marker;
    if(!abort_marker || w->r.state==ACCT_OWNER_ABORTED) {
        if(!irrevocable || w->r.state==ACCT_OWNER_COMMITTED) return -EIO;
        size_t bytes;
        int visible=read_db(s,DB,w->current_bytes,&w->current,&bytes);
        if((visible && visible!=-ENOENT) || (!visible && w->current.generation==w->q.transaction)) return -EIO;
        e=acct_owner_call(s,ACCT_OWNER_ABORT,&w->q,&w->r);
        if(!e) e=cleanup(s,w);
        return e?e:-ECANCELED;
    }
    /* An installed G without its required receipt cannot be legitimized by
     * a new PREPARE. Only matching COMMIT_REQUIRED permits pending-owner repair. */
    size_t visible_n;
    int visible_error=read_db(s,DB,w->current_bytes,&w->current,&visible_n);
    if(visible_error && visible_error!=-ENOENT) return visible_error;
    if(!visible_error && w->current.generation==w->q.transaction &&
        (w->r.state==ACCT_OWNER_NONE || w->r.state==ACCT_OWNER_ABORTED ||
         (w->r.state==ACCT_OWNER_PENDING && irrevocable==-ENOENT))) return -EIO;
    e=acct_boot_result(s,ACCT_BOOT_VALIDATE_STAGE,s->owner.validate_stage(s->owner.ctx,&w->q)); if(e) return e;
    if(w->r.state==ACCT_OWNER_NONE) return -EIO;
    if(w->r.state==ACCT_OWNER_RESERVED) {
        e=acct_owner_call(s,ACCT_OWNER_PREPARE,&w->q,&w->r); if(e) return e;
    }
    if(w->r.state==ACCT_OWNER_PENDING) {
        e=marker(s,w,"acct.prepared",1); if(e) return e;
        e=durable(s,NEW,w->bytes,w->db_bytes); if(e) return e;
        if(irrevocable==-ENOENT && s->subject && !s->kernel.peer_alive(s->kernel.ctx,s->subject)) {
            e=marker(s,w,"acct.abort",1);
            if(!e) e=acct_owner_call(s,ACCT_OWNER_ABORT,&w->q,&w->r);
            if(!e) e=cleanup(s,w);
            return e?e:-ESRCH;
        }
        /* After this barrier cancellation suppresses replies, never rolls back. */
        e=marker(s,w,"acct.commit",1); if(e) return e;
        e=acct_owner_call(s,ACCT_OWNER_COMMIT,&w->q,&w->r); if(e) return e;
    }
    if(w->r.state!=ACCT_OWNER_COMMITTED) return -EIO;
    e=marker(s,w,"acct.owner",1); if(e) return e;
    if(!w->q.expected_accounts_generation) { e=advance_boot(s,w,ACCT_OWNERS_READY); if(e) return e; }
    size_t n;
    e=read_db(s,DB,w->current_bytes,&w->current,&n);
    if(!e && w->current.generation==w->next.generation) {
        if(n!=w->db_bytes || memcmp(w->current_bytes,w->bytes,n)) return -EIO;
    } else {
        if(e && e!=-ENOENT) return e;
        if(!e) {
            if(w->current.generation!=w->q.expected_accounts_generation || memcmp(w->current_bytes+64,w->intent+32,32)) return -EIO;
            e=s->io.rename(s->io.ctx,DB,BAK); if(!e) e=s->io.barrier(s->io.ctx); if(e) return e;
        } else if(w->q.expected_accounts_generation) {
            e=read_db(s,BAK,w->current_bytes,&w->current,&n);
            if(e || w->current.generation!=w->q.expected_accounts_generation || memcmp(w->current_bytes+64,w->intent+32,32)) return -EIO;
        }
        /* Immutable intent contains the complete flushed staging; never derive
         * a new verifier during recovery. Missing .new is reproducible from it. */
        e=durable(s,NEW,w->bytes,w->db_bytes);
        if(!e) e=s->io.rename(s->io.ctx,NEW,DB);
        if(!e) e=s->io.flush(s->io.ctx,DB); if(!e) e=s->io.barrier(s->io.ctx); if(e) return e;
    }
    e=bindings(s,&w->next); if(!e) e=advance_boot(s,w,ACCT_CONSUMED);
    if(!e) e=marker(s,w,"acct.done",1);
    if(!e) e=cleanup(s,w);
    return e;
}
int acct_store_commit(struct acct_store *s, const struct acct_db *old, const struct acct_db *next,
                      const struct acct_owner_request *given, unsigned count)
{
    if(count>1 || !next || (old && (next->generation<=old->generation || next->next_uid<old->next_uid ||
        memcmp(old->sid,next->sid,12) || old->origin!=next->origin || (old->flags&4 && !(next->flags&4))))) return -EINVAL;
    struct workspace *w=alloc(s); if(!w) return -ENOMEM;
    int e=acct_encode(next,0,w->bytes,sizeof(w->bytes),&w->db_bytes); size_t n;
    uint8_t probe[1];
    if(!e) { int found=s->io.read(s->io.ctx,TXN,probe,1,&n); if(found!=-ENOENT) e=-EBUSY; }
    if(!e && old) {
        /* The workspace's current/expected records serve as scratch here;
         * both are rewritten before any later use and wiped by finish. */
        struct acct_db *d=&w->current; uint8_t *b=w->current_bytes, *expected=w->expected; size_t m;
        e=read_db(s,DB,b,d,&n);
        if(!e) e=acct_encode(old,0,expected,ACCT_FILE_MAX,&m);
        if(!e && (n!=m || memcmp(b,expected,n))) e=-EAGAIN;
        if(!e) memcpy(w->intent+32,b+64,32);
        acct_wipe(d,sizeof(*d)); acct_wipe(b,ACCT_FILE_MAX); acct_wipe(expected,ACCT_FILE_MAX);
    }
    if(!e && old) e=acct_owner_checkpoint(s,old,0);
    if(!e) {
        unsigned c; if(given && count) w->q=*given; else e=acct_owner_requests(s,next,&w->q,&c);
        if(!e && (w->q.count>32 || !w->q.count)) e=-EINVAL;
        w->q.expected_accounts_generation=old?old->generation:0;
        /* Retired roots remain in the owner store and are never reassigned. */
        if(old) for(unsigned i=0;!e && i<old->count;++i) {
            unsigned j=0; while(j<next->count && next->records[j].uid!=old->records[i].uid) ++j;
            if(j<next->count) continue;
            if(w->q.count==32) { e=-ENOSPC; break; }
            struct acct_owner_binding *b=&w->q.bindings[w->q.count++];
            e=s->owner.binding(s->owner.ctx,s->volume,old->records[i].uid,b->root,b); b->action=ACCT_OWNER_RETIRE; b->locator=0;
        }
        for(unsigned i=1;!e && i<w->q.count;++i) {
            struct acct_owner_binding b=w->q.bindings[i]; unsigned j=i;
            while(j && w->q.bindings[j-1].uid>b.uid) { w->q.bindings[j]=w->q.bindings[j-1]; --j; } w->q.bindings[j]=b;
        }
        if(!e && (w->q.count>32 || w->q.volume!=s->volume || w->q.transaction!=next->generation ||
            memcmp(w->q.sid,next->sid,12))) e=-EINVAL;
        for(unsigned i=0;!e && i<next->count;++i) {
            unsigned found=0;
            for(unsigned j=0;j<w->q.count;++j)
                if(w->q.bindings[j].uid==next->records[i].uid && w->q.bindings[j].action!=ACCT_OWNER_RETIRE) ++found;
            if(found!=1) e=-EINVAL;
        }
        for(unsigned j=0;!e && j<w->q.count;++j) {
            unsigned found=0;
            if(w->q.bindings[j].action==ACCT_OWNER_RETIRE) {
                if(old) for(unsigned i=0;i<old->count;++i) found+=old->records[i].uid==w->q.bindings[j].uid;
            } else for(unsigned i=0;i<next->count;++i) found+=next->records[i].uid==w->q.bindings[j].uid;
            if(found!=1) e=-EINVAL;
        }
    }
    if(!e) {
        uint8_t *manifest=w->manifest; size_t mn=0; char path[128];
        e=acct_boot_result(s,ACCT_BOOT_STAGE,s->owner.stage(s->owner.ctx,&w->q,manifest,ACCT_MANIFEST_MAX,&mn));
        if(!e) e=acct_manifest_validate(&w->q,manifest,mn);
        for(unsigned i=0;!e && i<w->q.count;++i) {
            uint8_t actual_tree[32]; e=acct_boot_result(s,ACCT_BOOT_TREE_DIGEST,s->owner.tree_digest(s->owner.ctx,w->q.transaction,w->q.bindings[i].uid,actual_tree));
            if(!e && memcmp(actual_tree,manifest+64+256*i+208,32)) e=-EIO;
            unsigned legacy=0;
            for(unsigned j=0;j<next->count;++j) if(next->records[j].uid==w->q.bindings[i].uid) legacy=next->records[j].legacy_id;
            if(a32(manifest+64+256*i+4)!=legacy && w->q.bindings[i].action==ACCT_OWNER_CREATE) e=-EACCES;
        }
        snprintf(path,sizeof(path),"/System/Security/Staging/g%016llx/manifest",(unsigned long long)w->q.transaction);
        if(!e) e=durable(s,path,manifest,mn);
        if(!e) sha256(manifest,mn,w->q.manifest_digest);
        acct_wipe(manifest,ACCT_MANIFEST_MAX);
    }
    if(!e) {
        sha256(w->bytes,w->db_bytes,w->q.target_digest);
        size_t qn; e=acct_owner_encode(&w->q,ACCT_OWNER_PREPARE,w->intent+192,ACCT_OWNER_BODY_MAX,&qn);
        if(!e) {
            memcpy(w->q.digest,w->intent+192+128,32);
            memcpy(w->intent,"CIACTXN3",8); w->intent_bytes=192+qn+w->db_bytes;
            w32(w->intent+8,(uint32_t)w->intent_bytes); w32(w->intent+12,(uint32_t)qn); w32(w->intent+16,(uint32_t)w->db_bytes);
            memcpy(w->intent+192+qn,w->bytes,w->db_bytes); sha256(w->intent,w->intent_bytes,w->intent+96);
            /* Durable reservation precedes any owner reservation/publication. */
            e=watermarks(s,next->sid,1,next->generation,next->next_uid);
        }
    }
    if(!e) e=acct_owner_call(s,ACCT_OWNER_RESERVE,&w->q,&w->r);
    if(!e) e=acct_boot_result(s,ACCT_BOOT_INTENT,durable(s,TXN,w->intent,w->intent_bytes));
    if(!e) { w->next=*next; e=run_intent(s,w); }
    return finish(s,w,e);
}
int acct_store_bootstrap(struct acct_store *s, struct acct_db *d)
{
    struct acct_bootstrap b; int e=acct_boot_read(s,&b); if(e) return e;
    if(!d || b.state!=ACCT_UNPROVISIONED || b.origin!=d->origin || b.volume!=s->volume) return -EIO;
    struct workspace *w=alloc(s); if(!w) return -ENOMEM;
    uint8_t digest[32]; size_t n;
    e=s->io.read(s->io.ctx,"provision.seed",w->seed,ACCT_FILE_MAX,&n);
    if(!e) e=acct_decode(&w->recipe,1,w->seed,n);
    if(!e) { sha256(w->seed,n,digest); if(memcmp(digest,b.seed_digest,32) || w->recipe.origin!=b.origin) e=-EIO; }
    if(!e) e=watermarks(s,d->sid,0,0,0);
    /* sec-user-02: a prior reservation is accepted only as the interrupted
     * bootstrap of this same SID intent (acct_store_recover checked it); the
     * reserved generation is retired and never reused. */
    if(!e && s->high_generation==UINT64_MAX) e=-EOVERFLOW;
    if(!e) { d->generation=s->high_generation+1; e=acct_store_commit(s,NULL,d,NULL,0); }
    return finish(s,w,e);
}
int acct_store_recover(struct acct_store *s, struct acct_db *d)
{
    int local=acct_store_local_recover(s); if(local) return local;
    struct workspace *w=alloc(s); if(!w) return -ENOMEM;
    struct acct_bootstrap b; int e=acct_boot_read(s,&b);
    if(e==-ENOENT) e=-EIO; /* missing evidence never authorizes a fresh recipe */
    if(e) return finish(s,w,e);
    e=s->io.read(s->io.ctx,TXN,w->intent,sizeof(w->intent),&w->intent_bytes);
    if(!e) {
        e=decode_intent(s,w); if(!e) e=run_intent(s,w);
        if(e==-ECANCELED) e=0;
        if(e) return finish(s,w,e);
        e=acct_boot_read(s,&b); if(e) return finish(s,w,e);
    } else if(e!=-ENOENT) return finish(s,w,e);
    e=acct_boot_result(s,ACCT_BOOT_ORPHANS,s->owner.recover_orphans(s->owner.ctx,s->volume,0)); if(e) return finish(s,w,e);
    size_t n; e=read_db(s,DB,w->bytes,&w->next,&n);
    if(b.state==ACCT_UNPROVISIONED) {
        if(e!=-ENOENT) return finish(s,w,-EIO);
        if(b.origin==ACCT_UPGRADE) return finish(s,w,-ENOENT);
        size_t en=0; uint64_t seq=0;
        int er=acct_blob_read(s,"enrollment.source",w->enrollment,sizeof(w->enrollment),&en,&seq);
        if(!er) return finish(s,w,-ENOENT);
        if(er!=-ENOENT) return finish(s,w,er);
        uint8_t digest[32]; uint64_t owner_g=0;
        e=s->io.read(s->io.ctx,"provision.seed",w->seed,ACCT_FILE_MAX,&n);
        if(!e) e=acct_decode(&w->recipe,1,w->seed,n);
        if(!e) { sha256(w->seed,n,digest); if(memcmp(digest,b.seed_digest,32) || w->recipe.origin!=b.origin) e=-EIO; }
        int identity=-ENOENT;
        if(!e) { identity=acct_boot_identity(s,&w->recipe,0); if(identity && identity!=-ENOENT) e=identity; }
        static const char *const absent[]={NEW,BAK};
        for(unsigned i=0;!e && i<sizeof(absent)/sizeof(*absent);++i) {
            int x=s->io.read(s->io.ctx,absent[i],w->bytes,sizeof(w->bytes),&n); if(x!=-ENOENT) e=-EIO;
        }
        /* sec-user-02 (contract 3.5/3.6, H08): a first boot interrupted after
         * its durable allocation watermark but before the account intent may
         * resume only with this installation's own SID intent (bootstrap.sid
         * bound to the seed digest/volume, machine.sid) and watermarks sealed
         * with that SID; the orphan owner reservation was aborted above and
         * bootstrap continues at the next generation. Any other prior
         * reservation stays contradictory and closed. */
        for(unsigned i=0;!e && i<2;++i) {
            int x=s->io.read(s->io.ctx,i?"accounts.alloc.1":"accounts.alloc.0",w->bytes,sizeof(w->bytes),&n);
            if(x==-ENOENT) continue;
            if(identity || watermarks(s,w->recipe.sid,0,0,0) || !s->high_generation) e=-EIO;
        }
        /* end sec-user-02 */
        if(!e) e=acct_boot_result(s,ACCT_BOOT_GENERATION,s->owner.generation(s->owner.ctx,&owner_g));
        if(!e && owner_g) e=-EIO;
        return finish(s,w,e?(e==-ENOENT?-EIO:e):-ENOENT);
    }
    if(e || b.state!=ACCT_CONSUMED || b.accounts_generation!=w->next.generation ||
        b.origin!=w->next.origin || memcmp(b.sid,w->next.sid,12) ||
        !!(b.flags&4)!=!!(w->next.flags&4)) return finish(s,w,-EIO);
    e=sid_copy(s,&w->next,0);
    if(!e) e=watermarks(s,w->next.sid,0,0,0);
    if(!e && (s->high_generation<w->next.generation || s->high_uid<w->next.next_uid)) e=-EIO;
    if(!e) e=bindings(s,&w->next);
    if(!e) { int backup=s->io.read(s->io.ctx,BAK,w->bytes,sizeof(w->bytes),&n); if(backup!=-ENOENT) e=-EIO; }
    if(!e) e=acct_owner_checkpoint_recover(s,&w->next);
    if(!e) *d=w->next;
    return finish(s,w,e);
}

int acct_store_load_watermarks(struct acct_store *s, const uint32_t sid[3])
{ return watermarks(s,sid,0,0,0); }
int acct_store_reserve_generation(struct acct_store *s, const uint32_t sid[3], uint64_t g, uint32_t uid)
{ return watermarks(s,sid,1,g,uid); }
/* Ownership-neutral changes have a complete local COMMIT_REQUIRED intent. The
 * owner checkpoint/bindings are checked, but no OWNER receipt is allocated. */
static int local_run(struct acct_store *s, struct workspace *w)
{
    if(w->intent_bytes<64 || w->intent_bytes>64+ACCT_FILE_MAX || memcmp(w->intent,"CIACTLC4",8) ||
        a32(w->intent+8)!=w->intent_bytes || a32(w->intent+12)) return -EIO;
    w->db_bytes=w->intent_bytes-64; memcpy(w->bytes,w->intent+64,w->db_bytes);
    int e=acct_decode(&w->next,0,w->bytes,w->db_bytes); if(e) return e;
    uint64_t old=a64(w->intent+16); if(!old || w->next.generation<=old) return -EIO;
    e=watermarks(s,w->next.sid,0,0,0);
    if(e || s->high_generation<w->next.generation || s->high_uid<w->next.next_uid) return -EIO;
    e=bindings(s,&w->next); if(e) return e;
    size_t n=0; e=read_db(s,DB,w->current_bytes,&w->current,&n);
    if(!e && w->current.generation==w->next.generation) {
        if(n!=w->db_bytes || memcmp(w->current_bytes,w->bytes,n)) return -EIO;
    } else {
        if(e && e!=-ENOENT) return e;
        if(!e) {
            if(w->current.generation!=old || memcmp(w->current_bytes+64,w->intent+32,32)) return -EIO;
            e=s->io.rename(s->io.ctx,DB,BAK); if(!e) e=s->io.barrier(s->io.ctx);
        } else {
            e=read_db(s,BAK,w->current_bytes,&w->current,&n);
            if(!e && (w->current.generation!=old || memcmp(w->current_bytes+64,w->intent+32,32))) e=-EIO;
        }
        if(!e) e=durable(s,NEW,w->bytes,w->db_bytes);
        if(!e) e=s->io.rename(s->io.ctx,NEW,DB);
        if(!e) e=s->io.flush(s->io.ctx,DB);
        if(!e) e=s->io.barrier(s->io.ctx);
        if(e) return e;
    }
    struct acct_bootstrap b; e=acct_boot_read(s,&b); if(e) return e;
    if(b.state!=ACCT_CONSUMED || (b.accounts_generation!=old && b.accounts_generation!=w->next.generation) ||
        memcmp(b.sid,w->next.sid,12) || ((b.flags&4) && !(w->next.flags&4))) return -EIO;
    b.transaction=b.accounts_generation=w->next.generation; b.next_uid=w->next.next_uid; b.flags|=w->next.flags&4;
    e=acct_boot_write(s,&b);
    if(!e) e=rm(s,BAK); if(!e) e=rm(s,NEW);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=rm(s,"accounts.local");
    if(!e) e=s->io.barrier(s->io.ctx); return e;
}
int acct_store_local_recover(struct acct_store *s)
{
    struct workspace *w=alloc(s); if(!w) return -ENOMEM;
    uint64_t seq=0; int e=acct_blob_read(s,"accounts.local",w->intent,sizeof(w->intent),&w->intent_bytes,&seq);
    if(!e) e=local_run(s,w); else if(e==-ENOENT) e=0;
    return finish(s,w,e);
}
int acct_store_local_commit(struct acct_store *s, const struct acct_db *old, const struct acct_db *next)
{
    if(!old || !next || next->count!=old->count || memcmp(next->sid,old->sid,12) ||
        next->origin!=old->origin || (old->flags&~next->flags) || next->next_uid<old->next_uid) return -EINVAL;
    for(unsigned i=0;i<old->count;++i) if(old->records[i].uid!=next->records[i].uid) return -EINVAL;
    struct workspace *w=alloc(s); if(!w) return -ENOMEM;
    int e=acct_encode(next,0,w->bytes,sizeof(w->bytes),&w->db_bytes);
    size_t n=0; if(!e) e=read_db(s,DB,w->current_bytes,&w->current,&n);
    if(!e && w->current.generation!=old->generation) e=-EAGAIN;
    if(!e) e=bindings(s,old);
    if(!e) e=acct_owner_checkpoint_recover(s,old);
    if(!e && s->subject && !s->kernel.peer_alive(s->kernel.ctx,s->subject)) e=-ESRCH;
    if(!e) e=watermarks(s,next->sid,1,next->generation,next->next_uid);
    if(!e) {
        memcpy(w->intent,"CIACTLC4",8); w->intent_bytes=64+w->db_bytes; w32(w->intent+8,(uint32_t)w->intent_bytes);
        w64(w->intent+16,old->generation); memcpy(w->intent+32,w->current_bytes+64,32); memcpy(w->intent+64,w->bytes,w->db_bytes);
        uint64_t seq=0; e=acct_blob_write(s,"accounts.local",w->intent,w->intent_bytes,&seq);
    }
    if(!e) e=local_run(s,w);
    return finish(s,w,e);
}
