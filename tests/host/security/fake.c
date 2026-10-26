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
    memcpy(p, f->files[i].data, f->files[i].size); *n = f->files[i].size;
    return 0;
}
static int write_file(void *ctx, const char *name, const void *p, size_t n)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    if (n > FAKE_FILE_BYTES) return -ENOSPC;
    int i = fake_file_index(f, name);
    if (i < 0) for (unsigned j = 0; j < FAKE_FILES; ++j)
        if (!f->files[j].name[0] && !f->files[j].durable_name[0]) { i = (int)j; break; }
    if (i < 0) return -ENOSPC;
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
    memcpy(f->files[i].durable_data, f->files[i].data, f->files[i].size);
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
    int yes = n < FAKE_PEERS && f->peers[n].alive && f->peers[n].epoch == s->retained >> 8 &&
        (!s->request_id || (f->peers[n].active_request==s->request_id && f->peers[n].cancelled_request!=s->request_id));
    pthread_mutex_unlock(&f->peer_lock);
    return yes;
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
static uint64_t locator(const char *root)
{ uint8_t sum[32]; sha256(root,strlen(root),sum); return a64(sum); }
static void manifest_proof(struct fake *f, struct sha256_ctx *hash)
{
    for(unsigned i=0;i<f->enrollment_count;++i) {
        uint8_t record[72]; w32(record,f->enrollment[i].legacy_id); w32(record+4,f->enrollment[i].uid);
        memcpy(record+8,f->enrollment[i].root,64); sha256_update(hash,record,sizeof(record));
    }
    if(f->move.uid) {
        uint8_t record[148]; w32(record,f->move.uid); w64(record+4,f->move.before); w64(record+12,f->move.after);
        memcpy(record+20,f->move.from,64); memcpy(record+84,f->move.to,64); sha256_update(hash,record,sizeof(record));
    }
}
static int root_proof(struct fake *f, const struct acct_owner_binding *b)
{
    if(b->action!=ACCT_OWNER_CREATE) return 0;
    char fresh[64]; snprintf(fresh,64,"/Windows/Profiles/u%08x",b->uid);
    for(unsigned i=0;i<f->enrollment_count;++i) {
        const struct acct_import_map *m=&f->enrollment[i];
        if(m->uid!=b->uid) continue;
        const char *expected=m->root;
        if(f->move.uid==b->uid) {
            if(strcmp(f->move.from,m->root) || f->move.before!=locator(f->move.from) ||
                f->move.after!=locator(f->move.to) || f->move.before==f->move.after) return -EIO;
            expected=f->move.to;
        }
        return strcmp(expected,b->root)?-EACCES:0;
    }
    return strcmp(fresh,b->root)?-EACCES:0;
}
static int stage(void *ctx, uint64_t volume, struct acct_owner_binding *b, unsigned count, uint8_t digest[32])
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    struct sha256_ctx hash; sha256_init(&hash);
    for(unsigned i=0;i<count;++i) {
        if(b[i].volume!=volume || !acct_profile_root_valid(b[i].root)) return -EINVAL;
        int proof=root_proof(f,&b[i]); if(proof) return proof;
        /* Fake destination resolver. Distinct canonical roots have stable ids. */
        uint8_t sum[32]; sha256(b[i].root,strlen(b[i].root),sum);
        uint64_t locator=a64(sum); if(!locator) return -EIO;
        if(b[i].locator && b[i].locator!=locator) return -EIO;
        b[i].locator=locator; sha256_update(&hash,b[i].root,64); sha256_update(&hash,&b[i].uid,4);
    }
    manifest_proof(f,&hash); sha256_final(&hash,digest); return event(f); /* destination flush barrier */
}
static int validate_stage(void *ctx, const struct acct_owner_request *q)
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    struct sha256_ctx hash; sha256_init(&hash);
    for(unsigned i=0;i<q->count;++i) {
        const struct acct_owner_binding *b=&q->bindings[i]; uint8_t sum[32]; sha256(b->root,strlen(b->root),sum);
        int proof=root_proof(f,b); if(proof) return proof;
        if(b->locator!=a64(sum)) return -EIO;
        sha256_update(&hash,b->root,64); sha256_update(&hash,&b->uid,4);
    }
    uint8_t digest[32]; manifest_proof(f,&hash); sha256_final(&hash,digest);
    return memcmp(digest,q->manifest_digest,32)?-EIO:0;
}
static int owner_call(void *ctx, const void *body, size_t n, void *reply, size_t cap, size_t *bytes)
{
    struct fake *f=ctx; if(f->failed) return -EIO;
    struct acct_owner_request q; unsigned op;
    int e=acct_owner_decode(&q,&op,body,n); if(e) return e;
    struct fake_owner_entry *entry=NULL;
    for(unsigned i=0;i<f->owner_used;++i) {
        struct acct_owner_request *old=&f->owners[i].request;
        if(old->transaction==q.transaction && old->volume==q.volume && !memcmp(old->sid,q.sid,12)) entry=&f->owners[i];
    }
    if(entry && memcmp(entry->request.digest,q.digest,32)) return -EIO;
    struct acct_owner_reply unknown={0}; memcpy(unknown.sid,q.sid,12); unknown.volume=q.volume;
    unknown.transaction=q.transaction; memcpy(unknown.digest,q.digest,32);
    if(op==ACCT_OWNER_STATUS) return acct_owner_reply_encode(entry?&entry->reply:&unknown,reply,cap,bytes);
    if(op==ACCT_OWNER_PREPARE) {
        ++f->prepare_calls;
        if(!entry) {
            if(q.expected_generation!=f->owner_generation) return -EAGAIN;
            if(f->owner_full || f->owner_used==FAKE_OWNERS) return -ENOSPC;
            unsigned creates=0;
            for(unsigned i=0;i<q.count;++i) {
                const struct acct_owner_binding *b=&q.bindings[i]; char root[64]={0}; struct acct_owner_binding old;
                int found=binding(f,q.volume,b->uid,root,&old);
                if(b->action==ACCT_OWNER_CREATE) {
                    if(found!=-ENOENT) return -EEXIST;
                    for(unsigned j=0;j<f->profile_count;++j)
                        if(b->locator==f->profiles[j].locator || !strcmp(b->root,f->profiles[j].root)) return -EEXIST;
                    ++creates;
                } else if(found || b->key!=old.key || b->generation!=old.generation || b->locator!=old.locator || strcmp(b->root,root)) return -EAGAIN;
            }
            if(f->profile_count+creates>32) return -ENOSPC;
            entry=&f->owners[f->owner_used++]; entry->request=q; entry->reply=unknown;
            entry->reply.count=q.count; entry->reply.state=ACCT_OWNER_PENDING;
            f->owner_high+=7; entry->reply.generation=f->owner_high;
            for(unsigned i=0;i<q.count;++i) {
                entry->reply.results[i].uid=q.bindings[i].uid; entry->reply.results[i].index=i;
                entry->reply.results[i].action=q.bindings[i].action;
                entry->reply.results[i].key=q.bindings[i].action==ACCT_OWNER_CREATE?++f->key_high:q.bindings[i].key;
                entry->reply.results[i].generation=q.bindings[i].action==ACCT_OWNER_KEEP?q.bindings[i].generation:f->owner_high;
            }
        }
        if(f->cancel_on_prepare) f->peers[1].alive=0;
    } else if(op==ACCT_OWNER_COMMIT) {
        ++f->commit_calls;
        if(!entry || entry->reply.state==ACCT_OWNER_ABORTED) return -EIO;
        if(entry->reply.state!=ACCT_OWNER_COMMITTED) {
            for(unsigned i=0;i<q.count;++i) {
                struct acct_owner_binding b=q.bindings[i];
                if(b.action==ACCT_OWNER_KEEP) continue;
                b.key=entry->reply.results[i].key; b.generation=entry->reply.results[i].generation;
                unsigned j=0; while(j<f->profile_count && f->profiles[j].uid!=b.uid) ++j;
                if(j==f->profile_count) ++f->profile_count;
                f->profiles[j]=b;
            }
            f->owner_generation=entry->reply.generation; entry->reply.state=ACCT_OWNER_COMMITTED;
        }
        if(f->revoke_after_commit) f->peers[1].alive=0;
    } else if(op==ACCT_OWNER_ABORT) {
        ++f->abort_calls;
        if(entry && entry->reply.state==ACCT_OWNER_COMMITTED) return -EBUSY;
        if(!entry) {
            if(f->owner_used==FAKE_OWNERS) return -ENOSPC;
            entry=&f->owners[f->owner_used++]; entry->request=q; entry->reply=unknown;
        }
        entry->reply.state=ACCT_OWNER_ABORTED;
    } else return -EINVAL;
    e=event(f); if(e) return e;
    return acct_owner_reply_encode(&entry->reply,reply,cap,bytes);
}
static int claim(void *ctx, const struct acct_subject *p, uint64_t attempt, uint32_t action,
                 uint32_t session, uint32_t console, uint64_t generation, uint32_t resolved_uid, uint32_t *uid)
{
    struct fake *f=ctx;
    if(!alive(f,p) || p->role!=ACCT_ROLE_UI || p->endpoint_generation!=f->attempt_endpoint_generation ||
        !attempt || attempt!=f->attempt || attempt<=f->consumed_attempt || console!=f->console_generation ||
        generation!=f->attempt_accounts_generation || (action==2?session!=0:session!=f->locked_session)) return -EAGAIN;
    if(action==2) f->intended_uid=resolved_uid;
    f->consumed_attempt=attempt; *uid=f->intended_uid; return 0;
}
static int outcome(void *ctx, const struct acct_subject *p, uint64_t attempt, int status, const void *v, size_t n)
{
    struct fake *f=ctx; const uint8_t *b=v;
    if(status) return status<0 && !n && attempt==f->consumed_attempt?0:-EINVAL;
    if(f->fail_outcome || !alive(f,p) || attempt!=f->consumed_attempt || n!=240 || a64(b+8)!=f->consumed_attempt || a32(b+16)!=p->endpoint_generation ||
        a32(b+20)!=f->console_generation || a64(b+24)!=f->attempt_accounts_generation) return -EAGAIN;
    struct acct_auth_outcome decoded;
    if(acct_outcome_decode(&decoded,0,b,n) || a32(decoded.credential+8)!=f->intended_uid) return -EAGAIN;
    memcpy(f->last_outcome,b,240); ++f->outcomes; return 0;
}
static int service_link(void *ctx, unsigned op, const struct acct_security_link *in, struct acct_security_link *out)
{
    struct fake *f=ctx; (void)out;
    if(!in || in->version!=1 || in->size!=64 || op<12 || op>14) return -EINVAL;
    if(op!=14) return -ENOSYS;
    if(in->endpoint_fd<0 || in->endpoint_fd>=FAKE_PEERS) return -EBADF;
    struct fake_peer *p=&f->peers[in->endpoint_fd];
    if(in->service!=p->subject.service || in->endpoint_generation!=p->subject.endpoint_generation) return -EAGAIN;
    p->cancelled_request=in->request_id_lo|((uint64_t)in->request_id_hi<<32); return 0;
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
    f->key_high=100; f->console_generation=1; f->attempt_endpoint_generation=1; f->active_uid = UINT32_MAX; f->now = 100;
    assert(!pthread_mutex_init(&f->peer_lock, NULL));
    for (unsigned i = 0; i < FAKE_PEERS; ++i) { f->peers[i].alive = 1; f->peers[i].epoch = 1; }
    f->peers[0].subject.group_count = 1;
    f->peers[0].subject.role=ACCT_ROLE_UI; f->peers[0].subject.service=ACCT_ENDPOINT_AUTH;
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
    free(f->owners);
}
struct acct_store fake_store(struct fake *f)
{
    struct acct_store s = {0};
    s.volume = 1234;
    s.io = (struct acct_io){f, 0, read_file, write_file, flush_file, barrier, rename_file, remove_file};
    s.kernel = (struct acct_kernel){f, 0, lease, unlease, retain, alive, release, active,
                                  clock_ms, entropy, allocate, deallocate, picture, claim, outcome, service_link, exchange_begin, deliver_reply, exchange_end};
    s.owner = (struct acct_owner){f, 0, owner_auth, generation, binding, stage, validate_stage, owner_call};
    return s;
}
void fake_powercut(struct fake *f)
{
    for (unsigned i = 0; i < FAKE_FILES; ++i) {
        strcpy(f->files[i].name, f->files[i].durable_name);
        f->files[i].size = f->files[i].durable_size;
        memcpy(f->files[i].data, f->files[i].durable_data, f->files[i].size);
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
int fake_run(struct acct_service *s, int endpoint, struct acct_request *q, struct acct_response *r)
{
    q->service=endpoint==0?ACCT_ENDPOINT_AUTH:ACCT_ENDPOINT_ACCOUNTS;
    if(q->operation==ACCT_AUTH || q->operation==ACCT_UNLOCK || q->operation==ACCT_ENROLL_VERIFY) {
        struct fake *f=s->store.kernel.ctx;
        q->attempt=++f->attempt; q->console=f->console_generation;
        q->session=q->operation==ACCT_UNLOCK?f->locked_session:0;
        f->attempt_accounts_generation=q->generation;
        f->attempt_endpoint_generation=f->peers[0].subject.endpoint_generation;
    }
    int e = acct_submit(s, endpoint, q, sizeof(*q), r);
    assert(!q->password[0] && !q->old_password[0] && !q->confirm[0]);
    if (!e) { assert(acct_work(s, r) == 1); e = r->error; }
    return e;
}
