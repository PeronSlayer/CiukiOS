/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int event(struct fake *f)
{
    if (f->failed) return -EIO;
    ++f->event;
    if (f->cut && f->event == f->cut) { f->failed = 1; return -EIO; }
    return 0;
}
int fake_file_index(struct fake *f, const char *name)
{
    for (unsigned i = 0; i < FAKE_FILES; ++i)
        if (!strcmp(f->files[i].name, name)) return (int)i;
    return -1;
}
static int read_file(void *ctx, const char *name, void *p, size_t cap, size_t *n)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    int i = fake_file_index(f, name);
    if (i < 0) return -ENOENT;
    if (f->files[i].size > cap) return -EIO;
    if(f->files[i].size) memcpy(p, f->files[i].data, f->files[i].size); *n = f->files[i].size;
    return 0;
}
static int write_file(void *ctx, const char *name, const void *p, size_t n)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    for(unsigned j=0;j<f->owner_used;++j) if(f->owners[j].reply.state==1 || f->owners[j].reply.state==5) {
        char pinned[128]; snprintf(pinned,sizeof(pinned),"/System/Security/Staging/g%016llx/manifest",(unsigned long long)f->owners[j].request.transaction);
        if(!strcmp(pinned,name)) return -EBUSY;
    }
    if (n > FAKE_FILE_BYTES) return -ENOSPC;
    int i = fake_file_index(f, name);
    if (i < 0) for (unsigned j = 0; j < FAKE_FILES; ++j)
        if (!f->files[j].name[0] && !f->files[j].durable_name[0]) { i = (int)j; break; }
    if (i < 0) return -ENOSPC;
    if(f->files[i].capacity<n) { f->files[i].data=realloc(f->files[i].data,n); assert(f->files[i].data); f->files[i].capacity=n; }
    strcpy(f->files[i].name, name);
    if (f->write_error && !strcmp(f->write_error_name, name)) {
        memcpy(f->files[i].data, p, n / 2); f->files[i].size = n / 2;
        return -f->write_error;
    }
    memcpy(f->files[i].data, p, n); f->files[i].size = n;
    return event(f);
}
static int flush_file(void *ctx, const char *name)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    int i = fake_file_index(f, name);
    if (i < 0) return -ENOENT;
    if(f->files[i].durable_capacity<f->files[i].size) {
        f->files[i].durable_data=realloc(f->files[i].durable_data,f->files[i].size); assert(f->files[i].durable_data);
        f->files[i].durable_capacity=f->files[i].size;
    }
    if(f->files[i].size) memcpy(f->files[i].durable_data, f->files[i].data, f->files[i].size);
    f->files[i].durable_size = f->files[i].size;
    return event(f);
}
static int barrier(void *ctx)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    for (unsigned i = 0; i < FAKE_FILES; ++i) strcpy(f->files[i].durable_name, f->files[i].name);
    return event(f);
}
static int rename_file(void *ctx, const char *from, const char *to)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    int i = fake_file_index(f, from);
    if (i < 0) return -ENOENT;
    if (fake_file_index(f, to) >= 0) return -EEXIST;
    strcpy(f->files[i].name, to);
    return event(f);
}
static int remove_file(void *ctx, const char *name)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    int i = fake_file_index(f, name);
    if (i < 0) return -ENOENT;
    f->files[i].name[0] = 0;
    return event(f);
}
static int lease(void *ctx)
{
    struct fake *f = ctx;
    if (f->deny_lease) return -EPERM;
    if (f->leased) return -EBUSY;
    f->leased = 1; return 0;
}
static void unlease(void *ctx) { ((struct fake *)ctx)->leased = 0; }
static int retain(void *ctx, int endpoint, struct acct_subject *s)
{
    struct fake *f = ctx;
    if (endpoint < 0 || endpoint >= FAKE_PEERS) return -EBADF;
    pthread_mutex_lock(&f->peer_lock);
    struct fake_peer *p = &f->peers[endpoint];
    int e = 0;
    if (!p->alive) e = -ESRCH;
    else {
        *s = p->subject;
        s->retained = ((uintptr_t)p->epoch << 8) | (unsigned)(endpoint + 1);
        if(f->distinct_handles) s->retained|=(uintptr_t)++f->retained_serial<<32;
        ++p->refs;
    }
    pthread_mutex_unlock(&f->peer_lock);
    return e;
}
static int alive(void *ctx, const struct acct_subject *s)
{
    struct fake *f = ctx;
    unsigned n = (s->retained & 255u) - 1;
    pthread_mutex_lock(&f->peer_lock);
    int yes = n < FAKE_PEERS && f->peers[n].alive && f->peers[n].epoch == ((s->retained >> 8)&0xffffffu) &&
        (!s->request_id || (f->peers[n].active_request==s->request_id && f->peers[n].cancelled_request!=s->request_id));
    pthread_mutex_unlock(&f->peer_lock);
    return yes;
}
static int same_link(void *ctx, const struct acct_subject *a, const struct acct_subject *b)
{
    return alive(ctx,a) && alive(ctx,b) && (uint32_t)a->retained==(uint32_t)b->retained &&
        a->endpoint_generation==b->endpoint_generation && a->service_generation==b->service_generation &&
        a->role_generation==b->role_generation;
}
static void release(void *ctx, struct acct_subject *s)
{
    struct fake *f = ctx;
    unsigned n = (s->retained & 255u) - 1;
    pthread_mutex_lock(&f->peer_lock);
    assert(n < FAKE_PEERS && f->peers[n].refs); --f->peers[n].refs;
    pthread_mutex_unlock(&f->peer_lock);
    acct_wipe(s, sizeof(*s));
}
static int active(void *ctx, uint32_t uid) { return ((struct fake *)ctx)->active_uid == uid; }
static uint64_t clock_ms(void *ctx) { return ((struct fake *)ctx)->now; }
static int entropy(void *ctx, void *p, size_t n)
{
    struct fake *f = ctx;
    if (++f->entropy_calls == f->fail_entropy) return -EAGAIN;
    unsigned char *bytes = p;
    for (size_t i = 0; i < n; ++i) bytes[i] = (unsigned char)(++f->sid_nonce);
    return 0;
}
static void *allocate(void *ctx, size_t n)
{
    struct fake *f = ctx;
    if (++f->allocations == f->fail_alloc) return NULL;
    void *p = malloc(n);
    if (p) ++f->outstanding;
    return p;
}
static void deallocate(void *ctx, void *p, size_t n)
{
    struct fake *f = ctx;
    const unsigned char *b = p;
    for (size_t i = 0; i < n; ++i) if (b[i]) f->deallocated_dirty = 1;
    assert(f->outstanding); --f->outstanding;
    free(p);
}
static int picture(void *ctx, uint32_t uid, const char *s)
{ (void)ctx; (void)uid; return !*s || !strcmp(s, "catalogue/ciuki.png"); }
static int owner_auth(void *ctx) { return ((struct fake *)ctx)->deny_owner ? -EACCES : 0; }
static int generation(void *ctx, uint64_t *g)
{ *g = ((struct fake *)ctx)->owner_generation; return 0; }
static int binding(void *ctx, uint64_t volume, uint32_t uid, char root[64], struct acct_owner_binding *r)
{
    struct fake *f=ctx;
    if(f->failed || f->wrong_owner) return -EIO;
    for(unsigned i=0;i<f->profile_count;++i) if(f->profiles[i].uid==uid && f->profiles[i].volume==volume) {
        if(f->profiles[i].action==ACCT_OWNER_RETIRE) return -EACCES;
        *r=f->profiles[i]; memcpy(root,r->root,64); return 0;
    }
    return -ENOENT;
}
static int root_occupied(void *ctx, uint64_t volume, const char *root)
{
    struct fake *f=ctx; if(f->failed || f->wrong_owner) return -EIO;
    for(unsigned i=0;i<f->profile_count;++i)
        if(f->profiles[i].volume==volume && !strcmp(f->profiles[i].root,root)) return 1;
    for(unsigned i=0;i<f->owner_used;++i) if(f->owners[i].reply.state==1 || f->owners[i].reply.state==5) {
        const struct acct_owner_request *q=&f->owners[i].request;
        for(unsigned j=0;j<q->count;++j)
            if(q->bindings[j].volume==volume && !strcmp(q->bindings[j].root,root)) return 1;
    }
    return 0;
}
static uint64_t locator(const char *root)
{ uint8_t sum[32]; sha256(root,strlen(root),sum); return a64(sum); }
static int root_proof(struct fake *f, const struct acct_owner_binding *b)
{
    if(b->action!=ACCT_OWNER_CREATE) return 0;
    char fresh[64]; snprintf(fresh,64,"/Windows/Profiles/u%08x",b->uid);
    for(unsigned i=0;i<f->enrollment_count;++i) if(f->enrollment[i].uid==b->uid) {
        const char *expected=f->enrollment[i].root;
        if(f->move.uid==b->uid) {
            if(strcmp(f->move.from,expected) || f->move.before!=locator(expected) || f->move.after!=locator(f->move.to)) return -EIO;
            expected=f->move.to;
        }
        return strcmp(expected,b->root)?-EACCES:0;
    }
    return strcmp(fresh,b->root)?-EACCES:0;
}
static int tree_digest(void *ctx, uint64_t transaction, uint32_t uid, uint8_t digest[32])
{
    struct fake *f=ctx; (void)transaction;
    uint8_t tree[106]={0}; w32(tree,1); w32(tree+5,1); size_t n=49;
    for(unsigned i=0;i<f->enrollment_count;++i) if(f->enrollment[i].uid==uid) {
        char content[32]; int length=snprintf(content,sizeof(content),"legacy-%u",f->enrollment[i].legacy_id);
        w32(tree+49,9); memcpy(tree+53,"data.txt",9); w32(tree+62,2); w64(tree+66,(uint64_t)length); sha256(content,(size_t)length,tree+74); n=106;
    }
    if(f->change_tree) return -EIO;
    return acct_tree_digest(tree,n,digest);
}
static int stage(void *ctx, const struct acct_owner_request *q, void *v, size_t cap, size_t *bytes)
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    *bytes=64+256*q->count; if(cap<*bytes) return -ENOSPC;
    uint8_t *p=v; memset(p,0,*bytes); memcpy(p,"CIUKISTG",8); w32(p+8,1); w32(p+12,64); w32(p+16,q->count);
    memcpy(p+24,q->sid,12); w64(p+40,q->volume); w64(p+48,q->transaction);
    uint8_t hash[32];
    for(unsigned i=0;i<q->count;++i) {
        const struct acct_owner_binding *b=&q->bindings[i]; int e=root_proof(f,b); if(e) return e;
        if(b->locator) return -EINVAL;
        uint8_t *r=p+64+256*i; w32(r,b->uid); w32(r+8,b->action); w32(r+12,1); memcpy(r+16,b->root,64);
        for(unsigned j=0;j<f->enrollment_count;++j) if(f->enrollment[j].uid==b->uid) {
            w32(r+4,f->enrollment[j].legacy_id); w32(r+12,strcmp(f->enrollment[j].root,b->root)?6:2); memcpy(r+80,f->enrollment[j].root,64);
        }
        snprintf((char *)r+144,64,"/System/Security/Staging/g%016llx/p%08x",(unsigned long long)q->transaction,b->uid);
        e=tree_digest(f,q->transaction,b->uid,hash); if(e) return e;
        memcpy(r+208,hash,32); w64(r+240,b->key);
    }
    if(f->corrupt_manifest) p[64+208]^=1;
    memcpy(f->manifest,p,*bytes); f->manifest_bytes=*bytes;
    return event(f);
}
static int validate_stage(void *ctx, const struct acct_owner_request *q)
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    char path[128]; snprintf(path,128,"/System/Security/Staging/g%016llx/manifest",(unsigned long long)q->transaction);
    uint8_t p[ACCT_MANIFEST_MAX],sum[32]; size_t n; int e=read_file(f,path,p,sizeof(p),&n);
    if(!e) e=acct_manifest_validate(q,p,n);
    if(!e) { sha256(p,n,sum); if(memcmp(sum,q->manifest_digest,32)) e=-EIO; }
    if(!e && (f->replace_root || f->change_tree)) e=-EIO;
    for(unsigned i=0;!e && i<q->count;++i) {
        e=root_proof(f,&q->bindings[i]); uint8_t tree[32];
        if(!e) e=tree_digest(f,q->transaction,q->bindings[i].uid,tree);
        if(!e && memcmp(tree,p+64+256*i+208,32)) e=-EIO;
    }
    return e;
}
static int owner_floor(void *ctx, uint64_t *floor, unsigned *count)
{ struct fake *f=ctx; *floor=f->owner_floor; *count=f->owner_used; return f->failed?-EIO:0; }
static int snapshot(void *ctx, const struct acct_db *d, uint64_t high, uint32_t uid_high, void *v, size_t cap, size_t *bytes, uint64_t *floor)
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    if(f->owner_used<=256) return -EAGAIN;
    unsigned remove=f->owner_used-256;
    for(unsigned i=0;i<f->owner_used;++i) if(f->owners[i].reply.state==1 || f->owners[i].reply.state==5) return -EAGAIN;
    *floor=f->owners[remove-1].request.transaction;
    if(*floor>high) return -EIO;
    uint8_t *p=v; if(cap<160+112*f->profile_count) return -ENOSPC;
    memset(p,0,160+112*f->profile_count); memcpy(p,"CIUKIOCP",8); w32(p+8,1); w32(p+12,160);
    w32(p+16,f->profile_count); w32(p+20,256); memcpy(p+24,d->sid,12); w64(p+40,1234); w64(p+48,*floor);
    w64(p+56,d->generation); uint8_t db[ACCT_FILE_MAX]; size_t n; int e=acct_encode(d,0,db,sizeof(db),&n); if(e) return e;
    sha256(db,n,p+64); w64(p+96,f->owner_generation); w64(p+104,f->owner_high); w64(p+112,f->key_high); w64(p+120,f->locator_high);
    w64(p+128,high); w32(p+136,uid_high);
    struct acct_owner_binding profiles[32]; memcpy(profiles,f->profiles,sizeof(profiles));
    for(unsigned i=1;i<f->profile_count;++i) { struct acct_owner_binding b=profiles[i]; unsigned j=i;
        while(j && profiles[j-1].uid>b.uid) { profiles[j]=profiles[j-1]; --j; } profiles[j]=b; }
    for(unsigned i=0;i<f->profile_count;++i) {
        struct acct_owner_binding *b=&profiles[i]; uint8_t *r=p+160+112*i; w32(r,b->uid); w32(r+4,b->action==2?2:1);
        w64(r+8,b->key); w64(r+16,b->generation); w64(r+24,b->volume); w64(r+32,b->locator); memcpy(r+40,b->root,64);
    }
    size_t offset=160+112*f->profile_count;
    for(unsigned i=remove;i<f->owner_used;++i) {
        size_t qn,rn; if(cap-offset<8+ACCT_OWNER_BODY_MAX+ACCT_OWNER_REPLY_MAX) return -ENOSPC;
        e=acct_owner_encode(&f->owners[i].request,1,p+offset+8,cap-offset-8,&qn); if(e) return e;
        e=acct_owner_reply_encode(&f->owners[i].reply,p+offset+8+qn,cap-offset-8-qn,&rn); if(e) return e;
        w32(p+offset,(uint32_t)qn); w32(p+offset+4,(uint32_t)rn); offset+=8+qn+rn;
    }
    *bytes=offset; return 0;
}
static int owner_call(void *ctx, const void *body, size_t n, void *reply, size_t cap, size_t *bytes)
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    struct acct_owner_request q; unsigned op; int e=acct_owner_decode(&q,&op,body,n); if(e) return e;
    struct acct_owner_reply unknown={0}; memcpy(unknown.sid,q.sid,12); unknown.volume=q.volume; unknown.transaction=q.transaction; memcpy(unknown.digest,q.digest,32);
    if(op==5 || op==6) {
        char path[128]; snprintf(path,128,"/System/Security/Staging/g%016llx/checkpoint",(unsigned long long)q.transaction);
        int i=fake_file_index(f,path); uint8_t sum[32]; if(i<0) return -EIO;
        sha256(f->files[i].data,f->files[i].size,sum); if(memcmp(sum,q.manifest_digest,32)) return -EIO;
        if(op==5) {
            if(f->checkpoint_floor_candidate==q.transaction && memcmp(f->checkpoint_candidate.digest,q.digest,32)) return -EIO;
            f->checkpoint_candidate=q; f->checkpoint_floor_candidate=q.transaction;
        } else {
            if(f->owner_floor!=q.transaction && (f->checkpoint_floor_candidate!=q.transaction || memcmp(f->checkpoint_candidate.digest,q.digest,32))) return -EIO;
            struct acct_store store=fake_store(f); uint8_t marker[192]; size_t mn; uint64_t seq=0;
            e=acct_blob_read(&store,"owner.checkpoint",marker,sizeof(marker),&mn,&seq);
            if(e || mn!=192 || memcmp(marker,body,8) || memcmp(marker+12,(const uint8_t *)body+12,148)) return -EIO;
            f->owner_floor=q.transaction; e=event(f); if(e) return e;
            unsigned remove=0; while(remove<f->owner_used && f->owners[remove].request.transaction<=f->owner_floor) ++remove;
            memmove(f->owners,f->owners+remove,(f->owner_used-remove)*sizeof(*f->owners)); f->owner_used-=remove;
        }
        e=event(f); if(e) return e; unknown.state=4; return acct_owner_reply_encode(&unknown,reply,cap,bytes);
    }
    if(q.transaction<=f->owner_floor) { if(op==2) return -EIO; unknown.state=4; return acct_owner_reply_encode(&unknown,reply,cap,bytes); }
    struct fake_owner_entry *entry=NULL;
    for(unsigned i=0;i<f->owner_used;++i) if(f->owners[i].request.transaction==q.transaction && f->owners[i].request.volume==q.volume && !memcmp(f->owners[i].request.sid,q.sid,12)) entry=&f->owners[i];
    if(entry && memcmp(entry->request.digest,q.digest,32)) return -EIO;
    if(op==4) return acct_owner_reply_encode(entry?&entry->reply:&unknown,reply,cap,bytes);
    if(op==7 && !entry) {
        ++f->reserve_calls;
        if(q.expected_generation!=f->owner_generation) return -EAGAIN;
        if(f->owner_full) return -ENOSPC;
        if(f->owner_used==1024) return -EAGAIN;
        for(unsigned i=0;i<f->owner_used;++i) if(f->owners[i].reply.state==1 || f->owners[i].reply.state==5) return -EBUSY;
        e=validate_stage(f,&q); if(e) return e; unsigned creates=0;
        for(unsigned i=0;i<q.count;++i) {
            const struct acct_owner_binding *b=&q.bindings[i]; char root[64]; struct acct_owner_binding old;
            if(b->locator) return -EINVAL;
            int found=binding(f,q.volume,b->uid,root,&old);
            if(b->action==1) {
                if(found!=-ENOENT) return -EEXIST;
                for(unsigned j=0;j<f->profile_count;++j) if(!strcmp(b->root,f->profiles[j].root)) return -EEXIST;
                ++creates;
            } else if(found || b->key!=old.key || b->generation!=old.generation || strcmp(b->root,root)) return -EAGAIN;
        }
        if(f->profile_count+creates>32) return -ENOSPC;
        entry=&f->owners[f->owner_used++]; memset(entry,0,sizeof(*entry)); entry->request=q; entry->reply=unknown;
        entry->reply.count=q.count; entry->reply.state=5; f->owner_high+=7; entry->reply.generation=f->owner_high;
        for(unsigned i=0;i<q.count;++i) {
            entry->reply.results[i].uid=q.bindings[i].uid; entry->reply.results[i].index=i; entry->reply.results[i].action=q.bindings[i].action;
            entry->reply.results[i].key=q.bindings[i].action==1?++f->key_high:q.bindings[i].key;
            entry->reply.results[i].generation=q.bindings[i].action==3?q.bindings[i].generation:f->owner_high;
            if(q.bindings[i].action==1) ++f->locator_high;
        }
    } else if(op==1) {
        ++f->prepare_calls;
        if(!entry || entry->reply.state==3) return -EIO;
        if(entry->reply.state==5) entry->reply.state=1;
        if(f->cancel_on_prepare) f->peers[1].alive=0;
    } else if(op==2) {
        ++f->commit_calls;
        if(!entry || entry->reply.state==3 || entry->reply.state==5) return -EIO;
        if(entry->reply.state!=2) {
            e=validate_stage(f,&q); if(e) return e;
            for(unsigned i=0;i<q.count;++i) {
                struct acct_owner_binding b=q.bindings[i]; if(b.action==3) continue;
                b.key=entry->reply.results[i].key; b.generation=entry->reply.results[i].generation;
                unsigned j=0; while(j<f->profile_count && f->profiles[j].uid!=b.uid) ++j;
                if(j==f->profile_count) { ++f->profile_count; b.locator=b.key-100; } else b.locator=f->profiles[j].locator;
                f->profiles[j]=b;
            }
            f->owner_generation=entry->reply.generation; entry->reply.state=2;
        }
        if(f->revoke_after_commit) f->peers[1].alive=0;
    } else if(op==3) {
        ++f->abort_calls; if(!entry) return acct_owner_reply_encode(&unknown,reply,cap,bytes);
        if(entry->reply.state==2) return -EBUSY; entry->reply.state=3;
    } else if(op!=7) return -EINVAL;
    e=event(f); if(e) return e; return acct_owner_reply_encode(&entry->reply,reply,cap,bytes);
}
static int orphans(void *ctx, uint64_t volume, uint64_t active_g)
{
    struct fake *f=ctx;
    for(unsigned i=0;i<f->owner_used;++i) if(f->owners[i].request.volume==volume && f->owners[i].request.transaction!=active_g &&
        (f->owners[i].reply.state==1 || f->owners[i].reply.state==5)) { f->owners[i].reply.state=3; int e=event(f); if(e) return e; }
    return 0;
}
static void subject_get(void *ctx, const struct acct_subject *s)
{ struct fake *f=ctx; unsigned i=(s->retained&255)-1; assert(i<FAKE_PEERS); ++f->peers[i].refs; }
static int roles(void *ctx, uint32_t *lg, uint32_t *ag)
{ struct fake *f=ctx; *lg=f->logon_generation; *ag=f->account_generation; return 0; }
static int gate_state(void *ctx, uint32_t session, uint32_t *console, uint32_t *auth, uint32_t *uid)
{ struct fake *f=ctx; if(session && session!=f->locked_session) return -EAGAIN;
    *console=f->console_generation; *auth=f->authentication_epoch; *uid=f->intended_uid; return 0; }
static int auth_binding(void *ctx, const struct acct_subject *p, uint32_t ui, uint32_t session, uint32_t console,
                        uint32_t auth, uint32_t lg, uint32_t ag, uint32_t *uid)
{
    struct fake *f=ctx;
    if(!alive(f,p) || p->role!=1 || (ui && ui!=f->attempt_endpoint_generation) || console!=f->console_generation || auth!=f->authentication_epoch ||
        lg!=f->logon_generation || ag!=f->account_generation || (session && session!=f->locked_session)) return -EAGAIN;
    *uid=f->intended_uid; return 0;
}
static int outcome(void *ctx, const struct acct_subject *p, uint64_t attempt, int status, const void *v, size_t n)
{
    struct fake *f=ctx; struct acct_auth_outcome decoded;
    if(f->fail_outcome || !alive(f,p) || status || n!=304 || acct_outcome_decode(&decoded,0,v,n) || decoded.challenge.attempt!=attempt ||
        decoded.challenge.console!=f->console_generation || decoded.challenge.authentication_epoch!=f->authentication_epoch ||
        decoded.challenge.logon_generation!=f->logon_generation || decoded.challenge.account_generation!=f->account_generation) return -EAGAIN;
    memcpy(f->last_outcome,v,n); f->consumed_attempt=attempt; ++f->outcomes; return 0;
}
static int enrollment_authorized(void *ctx, const struct acct_subject *p)
{ struct fake *f=ctx; return p->role==1 && !f->deny_enrollment && f->active_uid==UINT32_MAX?0:-EACCES; }
static int session_lock(void *ctx, const struct acct_subject *p, uint32_t session)
{ struct fake *f=ctx; if(!alive(f,p) || p->role!=6 || p->session_id!=session) return -EACCES;
    if(!f->locked) { ++f->console_generation; ++f->authentication_epoch; f->locked=1; } return 0; }
static int service_link(void *ctx, unsigned op, const struct acct_security_link *in, struct acct_security_link *out)
{
    struct fake *f=ctx; (void)out;
    if(!in || in->version!=1 || in->size!=64 || op<12 || op>14) return -EINVAL;
    if(op!=14) return -ENOSYS;
    if(in->endpoint_fd<0 || in->endpoint_fd>=FAKE_PEERS) return -EBADF;
    struct fake_peer *p=&f->peers[in->endpoint_fd];
    if(in->service!=p->subject.service || in->endpoint_generation!=p->subject.endpoint_generation) return -EAGAIN;
    p->cancelled_request=in->request_id_lo|((uint64_t)in->request_id_hi<<32);
    if(p->queued_request==p->cancelled_request && p->active_request!=p->queued_request) { p->queued_request=0; return 1; }
    return 0;
}
static int exchange_begin(void *ctx, const struct acct_subject *p, uint64_t id)
{
    struct fake *f=ctx; unsigned n=(p->retained&255)-1;
    if(!alive(f,p)) return -ESRCH;
    if(f->peers[n].active_request) return -EAGAIN;
    unsigned total=0; for(unsigned i=0;i<FAKE_PEERS;++i) total+=!!f->peers[i].active_request;
    if(total==8) return -EAGAIN;
    if(!id || id<=f->peers[n].last_request) return -EINVAL;
    f->peers[n].last_request=f->peers[n].active_request=id; return 0;
}
static int deliver_reply(void *ctx, const struct acct_subject *p, unsigned op, uint64_t gen, int status, const void *body, size_t n)
{
    if(!alive(ctx,p)) return -ESRCH;
    if(status && n) return -EINVAL;
    uint8_t frame[256]; size_t bytes=0;
    for(size_t offset=0;;offset+=192) {
        int e=acct_reply_frame(p,op,p->request_id,gen,status,body,n,offset,frame,&bytes); if(e) return e;
        if(offset+192>=n) break;
    }
    return 0;
}
static void exchange_end(void *ctx, const struct acct_subject *p)
{
    struct fake *f=ctx; unsigned n=(p->retained&255)-1;
    if(n<FAKE_PEERS && p->request_id && f->peers[n].active_request==p->request_id) f->peers[n].active_request=0;
}
void fake_init(struct fake *f)
{
    memset(f, 0, sizeof(*f)); f->owners=calloc(FAKE_OWNERS,sizeof(*f->owners)); assert(f->owners);
    f->key_high=100; f->logon_generation=f->account_generation=f->authentication_epoch=1; f->console_generation=1; f->attempt_endpoint_generation=1; f->active_uid = UINT32_MAX; f->now = 100;
    assert(!pthread_mutex_init(&f->peer_lock, NULL));
    for (unsigned i = 0; i < FAKE_PEERS; ++i) { f->peers[i].alive = 1; f->peers[i].epoch = 1; }
    f->peers[0].subject.group_count = 1;
    f->peers[0].subject.role=ACCT_ROLE_LOGON; f->peers[0].subject.service_generation=1; f->peers[0].subject.role_generation=1; f->peers[0].subject.service=ACCT_ENDPOINT_AUTH;
    f->peers[1].subject.uid = 1000; f->peers[1].subject.group_count = 2;
    f->peers[1].subject.groups[0] = 544; f->peers[1].subject.groups[1] = 545;
    f->peers[2].subject.uid = 1001; f->peers[2].subject.group_count = 1; f->peers[2].subject.groups[0] = 545;
    f->peers[3].subject.uid = 501; f->peers[3].subject.group_count = 1; f->peers[3].subject.groups[0] = 546;
    for(unsigned i=4;i<FAKE_PEERS;++i) {
        f->peers[i].subject.uid=1002; f->peers[i].subject.group_count=1; f->peers[i].subject.groups[0]=545;
    }
    for(unsigned i=0;i<FAKE_PEERS;++i) f->peers[i].subject.endpoint_generation=i+1;
    for (unsigned i = 1; i < FAKE_PEERS; ++i) { f->peers[i].subject.session_id = 1; f->peers[i].subject.service=ACCT_ENDPOINT_ACCOUNTS; }
}
void fake_destroy(struct fake *f)
{
    assert(!f->outstanding && !f->deallocated_dirty);
    for (unsigned i = 0; i < FAKE_PEERS; ++i) assert(!f->peers[i].refs);
    assert(!pthread_mutex_destroy(&f->peer_lock));
    for(unsigned i=0;i<FAKE_FILES;++i) { free(f->files[i].data); free(f->files[i].durable_data); }
    free(f->owners);
}
struct acct_store fake_store(struct fake *f)
{
    struct acct_store s = {0};
    s.volume = 1234;
    s.io = (struct acct_io){f, 0, read_file, write_file, flush_file, barrier, rename_file, remove_file};
    s.kernel = (struct acct_kernel){.ctx=f, .lease_acquire=lease,.lease_release=unlease,.retain_peer=retain,.subject_get=subject_get,
        .peer_alive=alive,.same_link=same_link,.release_peer=release,.active_uid=active,.monotonic_ms=clock_ms,.entropy=entropy,.allocate=allocate,.deallocate=deallocate,
        .picture_valid=picture,.auth_binding=auth_binding,.gate_state=gate_state,.role_generations=roles,.auth_outcome=outcome,.enrollment_authorized=enrollment_authorized,
        .session_lock=session_lock,.service_link=service_link,.exchange_begin=exchange_begin,.deliver_reply=deliver_reply,.exchange_end=exchange_end};
    s.owner = (struct acct_owner){.ctx=f,.authenticate=owner_auth,.generation=generation,.binding=binding,.root_occupied=root_occupied,.stage=stage,.tree_digest=tree_digest,.validate_stage=validate_stage,
        .snapshot=snapshot,.recover_orphans=orphans,.checkpoint_floor=owner_floor,.call=owner_call};
    return s;
}
void fake_powercut(struct fake *f)
{
    for (unsigned i = 0; i < FAKE_FILES; ++i) {
        strcpy(f->files[i].name, f->files[i].durable_name);
        f->files[i].size = f->files[i].durable_size;
        if(f->files[i].capacity<f->files[i].size) { f->files[i].data=realloc(f->files[i].data,f->files[i].size); assert(f->files[i].data); f->files[i].capacity=f->files[i].size; }
        if(f->files[i].size) memcpy(f->files[i].data, f->files[i].durable_data, f->files[i].size);
    }
    f->failed = f->cut = 0;
}
void fake_put(struct fake *f, const char *name, const void *p, size_t n)
{ assert(!write_file(f, name, p, n)); assert(!flush_file(f, name)); assert(!barrier(f)); }
void fake_seed(struct acct_db *d, int ready)
{
    memset(d, 0, sizeof(*d)); d->generation = 0; d->next_uid = 1001; d->count = 3;
    d->origin = ready ? ACCT_READY : ACCT_SETUP; d->auto_logon_uid = ready ? 1000 : 0;
    d->records[0].uid = 500; d->records[0].groups = ACCT_ADMIN; d->records[0].flags = ACCT_BUILTIN;
    strcpy(d->records[0].name, "CiukiAdministrator");
    d->records[1].uid = 501; d->records[1].groups = ACCT_GUEST; d->records[1].flags = ACCT_BUILTIN;
    strcpy(d->records[1].name, "Guest");
    d->records[2].uid = 1000; d->records[2].groups = ACCT_ADMIN; d->records[2].flags = ACCT_ENABLED;
    strcpy(d->records[2].name, ready ? "CiukiUser" : "Alice");
    if (ready) d->records[2].flags |= ACCT_READY_BLANK;
    else for (unsigned i = 0; i < 3; i += 2) {
        struct acct_record *r = &d->records[i];
        r->flags |= ACCT_PASSWORD_SET; r->kdf = 1; r->iterations = ACCT_ITERATIONS;
        for (unsigned j = 0; j < 16; ++j) r->salt[j] = (uint8_t)(j+i);
        assert(!acct_derive("password", r->salt, ACCT_ITERATIONS, r->verifier));
    }
}
void fake_provision(struct fake *f, int ready)
{
    struct acct_db seed; fake_seed(&seed,ready); uint8_t bytes[ACCT_FILE_MAX], state[160]; size_t n;
    assert(!acct_encode(&seed,1,bytes,sizeof(bytes),&n));
    fake_put(f,"provision.seed",bytes,n);
    struct acct_bootstrap b={0}; b.state=1; b.origin=ready?1:2; b.volume=1234; b.next_uid=1001; b.generation=1;
    sha256(bytes,n,b.seed_digest); assert(!acct_boot_encode(&b,state,160)); fake_put(f,"bootstrap.state",state,160);
}
void fake_boot(struct fake *f, struct acct_service *s)
{
    fake_provision(f,1); struct acct_store store=fake_store(f); assert(!acct_service_open(s,&store,1));
    struct acct_db seed; fake_seed(&seed,1); uint8_t bytes[ACCT_FILE_MAX]; size_t n;
    assert(!acct_encode(&seed,1,bytes,sizeof(bytes),&n)); assert(!acct_service_bootstrap(s,bytes,n,ACCT_READY));
    acct_wipe(&seed,sizeof(seed)); acct_wipe(bytes,sizeof(bytes));
}
void fake_request(struct acct_request *q, unsigned op, uint64_t gen, uint32_t uid, const char *name, const char *password)
{
    memset(q, 0, sizeof(*q)); q->operation = op; q->generation = gen; q->uid = uid;
    if (name) strcpy(q->name, name);
    if (password) { strcpy(q->password, password); strcpy(q->confirm, password); }
}
int fake_challenge(struct acct_service *s, int endpoint, struct acct_request *q)
{
    struct fake *f=s->store.kernel.ctx; struct acct_request c={0}; struct acct_response r;
    unsigned action=q->operation==ACCT_UNLOCK?3:q->operation==ACCT_ENROLL_VERIFY?4:q->operation==ACCT_AUTOLOGON?5:2;
    c.operation=ACCT_CHALLENGE; c.service=1; c.begin.action=action;
    c.begin.selector=q->name[0]?2:1; c.begin.uid=q->name[0]?0:q->uid; memcpy(c.begin.name,q->name,32);
    c.challenge.action=action; c.challenge.attempt=++f->attempt; c.challenge.console=f->console_generation; c.challenge.authentication_epoch=f->authentication_epoch;
    c.challenge.ui_generation=action==5?0:f->attempt_endpoint_generation; c.challenge.logon_generation=f->logon_generation; c.challenge.account_generation=f->account_generation;
    c.challenge.flags=action==5?1:0; c.challenge.session=c.begin.session=action==3?f->locked_session:0; c.begin.console=action==3?f->console_generation:0;
    int e=acct_submit(s,endpoint,&c,sizeof(c),&r);
    if(!e) { assert(acct_work(s,&r)==1); e=r.error; }
    if(!e) { assert(!acct_challenge_decode(&q->challenge,0,r.body,r.body_bytes)); q->generation=q->challenge.accounts_generation;
        q->uid=q->challenge.uid; q->attempt=q->challenge.attempt; q->console=q->challenge.console; q->session=q->challenge.session; }
    return e;
}
int fake_reserve(struct acct_service *s, int endpoint, struct acct_request *q)
{
    switch(q->operation) {
    case ACCT_PASSWORD: q->wire_operation=2; break;
    case ACCT_CREATE: q->wire_operation=3; break;
    case ACCT_DELETE: q->wire_operation=5; break;
    case ACCT_RENAME: q->operation=ACCT_UPDATE; q->selectors=1024; q->flags=1024; q->wire_operation=4; break;
    case ACCT_PICTURE: q->operation=ACCT_UPDATE; q->selectors=4096; q->flags=4096; q->wire_operation=4; break;
    case ACCT_ENABLE: q->operation=ACCT_UPDATE; q->selectors=256; q->flags|=256; q->wire_operation=4; break;
    case ACCT_ROLE: q->operation=ACCT_UPDATE; q->selectors=512; q->flags=512|(q->groups==ACCT_ADMIN?2:0); q->wire_operation=4; break;
    case ACCT_UPDATE: q->wire_operation=4; break;
    default: return -EINVAL;
    }
    struct acct_request reserve={0}; struct acct_response r; reserve.operation=ACCT_RESERVE; reserve.service=2; reserve.flags=q->wire_operation;
    assert(!acct_management_digest(q,reserve.request_digest));
    int e=acct_submit(s,endpoint,&reserve,sizeof(reserve),&r);
    if(!e) { assert(acct_work(s,&r)==1); e=r.error; }
    if(!e) q->client_operation_id=a64(r.body);
    return e;
}
int fake_run(struct acct_service *s, int endpoint, struct acct_request *q, struct acct_response *r)
{
    q->service=endpoint==0?1:2;
    int e=0;
    if(q->operation==ACCT_AUTH || q->operation==ACCT_UNLOCK || q->operation==ACCT_ENROLL_VERIFY || q->operation==ACCT_AUTOLOGON) { if(!q->challenge.attempt) e=fake_challenge(s,endpoint,q); }
    else if(q->operation!=ACCT_LIST && q->operation!=ACCT_LOOKUP && q->operation!=ACCT_STATUS && q->operation!=ACCT_RESERVE && q->operation!=ACCT_PROFILE && q->operation!=ACCT_ENROLL_PLAN && !q->client_operation_id) e=fake_reserve(s,endpoint,q);
    if(e) { memset(r,0,sizeof(*r)); r->error=e; r->retry_after_ms=e==-EACCES?1000:0; acct_wipe(q->password,64); acct_wipe(q->old_password,64); acct_wipe(q->confirm,64); return e; }
    e=acct_submit(s,endpoint,q,sizeof(*q),r);
    assert(!q->password[0] && !q->old_password[0] && !q->confirm[0]);
    if(!e) { assert(acct_work(s,r)==1); e=r->error;
        if(!e && r->body_bytes==16 && a32(r->body)==1 && a32(r->body+4)==4) e=(int32_t)a32(r->body+8); }
    return e;
}
