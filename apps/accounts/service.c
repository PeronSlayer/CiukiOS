/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int try_lock(atomic_flag *f)
{ return !atomic_flag_test_and_set_explicit(f, memory_order_acquire); }
static void unlock(atomic_flag *f)
{ atomic_flag_clear_explicit(f, memory_order_release); }
static void clear_passwords(struct acct_request *q)
{
    acct_wipe(q->password, sizeof(q->password));
    acct_wipe(q->confirm, sizeof(q->confirm));
    acct_wipe(q->old_password, sizeof(q->old_password));
}
static int adapter_valid(const struct acct_store *s)
{
    const struct acct_kernel *k = &s->kernel;
    return s->volume && k->lease_acquire && k->lease_release && k->retain_peer &&
        k->subject_get && k->peer_alive && k->same_link && k->release_peer && k->active_uid && k->monotonic_ms &&
        k->entropy && k->allocate && k->deallocate && k->picture_valid &&
        k->auth_binding && k->role_generations && k->auth_outcome && k->enrollment_authorized && k->session_lock && k->service_link &&
        k->exchange_begin && k->exchange_end && k->deliver_reply &&
        s->owner.authenticate && s->owner.generation && s->owner.binding && s->owner.root_occupied && s->owner.stage && s->owner.tree_digest && s->owner.validate_stage && s->owner.snapshot && s->owner.checkpoint_floor && s->owner.recover_orphans && s->owner.call &&
        s->io.read && s->io.write && s->io.flush && s->io.barrier && s->io.rename && s->io.remove;
}
static int pictures_valid(const struct acct_kernel *k, const struct acct_db *d)
{
    for (unsigned i = 0; i < d->count; ++i)
        if (!k->picture_valid(k->ctx, d->records[i].uid, d->records[i].picture)) return 0;
    return 1;
}
int acct_service_open(struct acct_service *s, const struct acct_store *store, int host_test)
{
    if (!s || !store || !adapter_valid(store)) return -EINVAL;
    memset(s, 0, sizeof(*s));
    atomic_flag_clear(&s->queue_lock); atomic_flag_clear(&s->worker_lock);
    atomic_init(&s->available, 0);
    if (!host_test && (!store->kernel.real || !store->owner.real || !store->io.real)) return -ENOSYS;
    s->store = *store;
    int e = store->kernel.lease_acquire(store->kernel.ctx);
    if (e) return e;
    s->leased = 1;
    e = acct_service_resume(s);
    if (e) {
        enum acct_boot_step step=s->store.firstboot_step;
        acct_service_close(s);
        s->store.firstboot_step=step;
    }
    return e;
}
int acct_service_resume(struct acct_service *s)
{
    if (!s || !s->leased || s->available || s->active.occupied || s->pending.occupied) return -EPERM;
    if (s->results) {
        acct_wipe(s->results,sizeof(*s->results));
        s->store.kernel.deallocate(s->store.kernel.ctx,s->results,sizeof(*s->results));
        s->results=NULL;
    }
    s->store.firstboot_step=ACCT_BOOT_RECOVER;
    int e = s->store.owner.authenticate(s->store.owner.ctx);
    if (!e) e = acct_store_recover(&s->store, &s->db);
    if (!e && !pictures_valid(&s->store.kernel, &s->db)) e = -EIO;
    if (!e) e=acct_results_open(s);
    if (!e) s->available = 1;
    else if (e != -ENOENT) return e;
    e=acct_enrollment_recover(s);
    if(e) { s->available=0; return e; }
    s->next_ticket = 1;
    return 0;
}
void acct_service_close(struct acct_service *s)
{
    if (!s) return;
    struct acct_kernel *k = &s->store.kernel;
    if (s->active.occupied) { k->exchange_end(k->ctx,&s->active.subject); k->release_peer(k->ctx, &s->active.subject); }
    if (s->pending.occupied) { k->exchange_end(k->ctx,&s->pending.subject); k->release_peer(k->ctx, &s->pending.subject); }
    acct_attempts_clear(s);
    if(s->results) { acct_wipe(s->results,sizeof(*s->results)); k->deallocate(k->ctx,s->results,sizeof(*s->results)); }
    if (s->leased) k->lease_release(k->ctx);
    acct_wipe(s, sizeof(*s));
}
int acct_service_bootstrap(struct acct_service *s, const void *seed, size_t bytes, uint32_t expected_origin)
{
    if (!s->leased || s->available) return -EPERM;
    if (!try_lock(&s->worker_lock)) return -EBUSY;
    struct acct_db decoded = {0};
    int e = acct_decode(&decoded, 1, seed, bytes);
    if (!e && (expected_origin < ACCT_READY || expected_origin > ACCT_SETUP ||
               decoded.origin != expected_origin)) e = -EINVAL;
    if (!e && !pictures_valid(&s->store.kernel, &decoded)) e = -EINVAL;
    if (!e) {
        struct acct_bootstrap b;
        uint8_t sum[32];
        e=acct_boot_read(&s->store,&b);
        sha256(seed,bytes,sum);
        if(!e && (b.state!=ACCT_UNPROVISIONED || b.origin!=expected_origin ||
            b.volume!=s->store.volume || memcmp(b.seed_digest,sum,32))) e=-EIO;
        acct_wipe(sum,sizeof(sum));
    }
    if (e) { acct_wipe(&decoded, sizeof(decoded)); unlock(&s->worker_lock); return e; }
    struct acct_kernel *k = &s->store.kernel;
    struct acct_db *d = k->allocate(k->ctx, sizeof(*d));
    if (d) *d = decoded;
    else e = -ENOMEM;
    acct_wipe(&decoded, sizeof(decoded));
    if (!e) {
        e=acct_boot_identity(&s->store,d,1);
        if (e && s->store.firstboot_step!=ACCT_BOOT_ENTROPY_SID) s->store.firstboot_step=ACCT_BOOT_IDENTITY;
    }
    if (!e && d->origin == ACCT_READY) {
        struct acct_record *r = &d->records[2];
        e = acct_boot_result(&s->store,ACCT_BOOT_ENTROPY_SALT,k->entropy(k->ctx, r->salt, 16));
        if (!e) e = acct_derive("", r->salt, ACCT_ITERATIONS, r->verifier);
        if (!e) { r->kdf = 1; r->iterations = ACCT_ITERATIONS; r->flags |= ACCT_PASSWORD_SET; }
    }
    if (!e) e = acct_store_bootstrap(&s->store, d);
    if (!e) { s->db = *d; e=acct_results_open(s); if(!e) s->available=1; }
    if (d) { acct_wipe(d, sizeof(*d)); k->deallocate(k->ctx, d, sizeof(*d)); }
    unlock(&s->worker_lock);
    return e;
}
static int admin(const struct acct_subject *subject)
{
    if (subject->uid == 0) return 0; /* uid 0 alone is never a role. */
    for (unsigned i = 0; i < subject->group_count && i < 8; ++i)
        if (subject->groups[i] == 544) return 1;
    return 0;
}
static int authentication(unsigned op)
{ return op == ACCT_AUTH || op == ACCT_UNLOCK || op == ACCT_ENROLL_VERIFY || op == ACCT_AUTOLOGON || op == ACCT_CHALLENGE || op == ACCT_ENROLL_PLAN ||
         op == ACCT_ENROLL_SOURCE; /* sec-user-04: LOGON-only route, no secret */ }
/* sec-user-04: the read-only source list is LOGON-only like authentication
 * but never waits for the failed-authentication retry delay. */
static int throttled(unsigned op)
{ return (authentication(op) && op != ACCT_ENROLL_SOURCE) || op == ACCT_PASSWORD; }
static uint32_t retry_ms(struct acct_service *s)
{
    uint64_t now = s->store.kernel.monotonic_ms(s->store.kernel.ctx);
    uint64_t delay = s->retry_at > now ? s->retry_at - now : 0;
    return delay > 1000 ? 1000 : (uint32_t)delay;
}
int acct_submit(struct acct_service *s, int endpoint, struct acct_request *q, size_t bytes,
                struct acct_response *out)
{
    if (!out) { if (q && bytes == sizeof(*q)) clear_passwords(q); return -EINVAL; }
    memset(out, 0, sizeof(*out));
    if (!q || bytes != sizeof(*q)) return out->error = -EINVAL;
    int e = 0;
    if (!s->leased || s->faulted || (!s->available && !(authentication(q->operation) || q->operation==ACCT_LIST))) e = -EIO;
    else if (q->operation < ACCT_LIST || q->operation > ACCT_ENROLL_SOURCE || /* sec-user-04 */
             !memchr(q->name, 0, 32) || !memchr(q->picture, 0, 96) ||
             !acct_password_valid(q->password, 1) || !acct_password_valid(q->confirm, 1) ||
             !acct_password_valid(q->old_password, 1)) e = -EINVAL;
    struct acct_subject subject = {0};
    struct acct_kernel *k = &s->store.kernel;
    if (!e) e = k->retain_peer(k->ctx, endpoint, &subject);
    int retained = !e;
    if(!e) subject.request_id=q->request_id;
    if (!e && (!subject.retained || !subject.group_count || subject.group_count > 8)) e = -EACCES;
    if (!e && (q->service != subject.service ||
        (subject.service != ACCT_ENDPOINT_AUTH && subject.service != ACCT_ENDPOINT_ACCOUNTS))) e=-EPERM;
    if (!e && subject.service==ACCT_ENDPOINT_AUTH && subject.role!=ACCT_ROLE_UI && subject.role!=ACCT_ROLE_LOGON) e=-EPERM;
    if (!e && subject.service==ACCT_ENDPOINT_AUTH && !authentication(q->operation) && q->operation!=ACCT_LIST) e=-EPERM;
    if (!e && q->self_password && subject.uid!=q->uid) e=-EPERM;
    if (!e && authentication(q->operation) &&
        (subject.role != ACCT_ROLE_LOGON || subject.service != ACCT_ENDPOINT_AUTH ||
         !subject.endpoint_generation || subject.uid || subject.session_id)) e = -EPERM;
    if (!e) {
        if (!try_lock(&s->queue_lock)) e = -EAGAIN;
        else {
            uint32_t retry = retry_ms(s);
            if (throttled(q->operation) && retry) {
                out->retry_after_ms = retry; e = -EAGAIN;
            } else if (!s->next_ticket) e = -EAGAIN;
            else {
                struct acct_job *job = !s->active.occupied ? &s->active :
                    authentication(q->operation) && subject.role==ACCT_ROLE_LOGON && !s->pending.occupied ? &s->pending : NULL;
                if (!job) { e = -EAGAIN; out->retry_after_ms = 1000; }
                else {
                    job->ticket = s->next_ticket++;
                    job->subject = subject; job->request = *q; job->occupied = 1;
                    out->ticket = job->ticket; retained = 0;
                }
            }
            unlock(&s->queue_lock);
        }
    }
    if (retained) k->release_peer(k->ctx, &subject);
    clear_passwords(q);
    out->error = e;
    return e;
}
static int find_uid(const struct acct_db *d, uint32_t uid)
{
    for (unsigned i = 0; i < d->count; ++i) if (d->records[i].uid == uid) return (int)i;
    return -1;
}
static int find_name(const struct acct_db *d, const char *name)
{
    for (unsigned i = 0; i < d->count; ++i) if (acct_name_equal(d->records[i].name, name)) return (int)i;
    return -1;
}
static void snapshot(const struct acct_db *d, const struct acct_subject *subject, struct acct_response *out)
{
    out->generation = d->generation;
    int management = admin(subject) && subject->service!=ACCT_ENDPOINT_AUTH;
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        if (!(r->flags & ACCT_ENABLED) && !management &&
            !(subject->service==ACCT_ENDPOINT_AUTH && subject->safe_mode && r->uid == 500 && (r->flags & ACCT_PASSWORD_SET))) continue;
        struct acct_public *p = &out->records[out->count++];
        p->uid = r->uid; p->flags = (r->flags & ACCT_ENABLED) | (r->groups == ACCT_ADMIN ? 2u : 0u); p->groups = r->groups;
        if (subject->service==ACCT_ENDPOINT_AUTH && (r->flags & ACCT_READY_BLANK))
            p->flags |= ACCT_PUBLIC_BLANK_LOGON;
        memcpy(p->name, r->name, 32); memcpy(p->picture, r->picture, 96);
    }
}
int acct_verify(const struct acct_record *r, const char *password)
{
    uint8_t salt[16] = {0}, expected[32] = {0}, actual[32];
    if (r && (r->flags & ACCT_PASSWORD_SET)) {
        memcpy(salt, r->salt, 16); memcpy(expected, r->verifier, 32);
    }
    int e = acct_derive(password, salt, ACCT_ITERATIONS, actual);
    int ok = !e && acct_equal_secret(expected, actual, 32);
    acct_wipe(salt, sizeof(salt)); acct_wipe(expected, sizeof(expected)); acct_wipe(actual, sizeof(actual));
    return r && (r->flags & ACCT_PASSWORD_SET) && ok;
}
static int has_administrator(const struct acct_db *d)
{
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        if (r->groups == ACCT_ADMIN && (r->flags & ACCT_ENABLED) &&
            (r->flags & ACCT_PASSWORD_SET)) return 1;
    }
    return 0;
}
int acct_set_password(struct acct_service *s, struct acct_record *r, const char *password)
{
    int e = s->store.kernel.entropy(s->store.kernel.ctx, r->salt, 16);
    if (!e) e = acct_derive(password, r->salt, ACCT_ITERATIONS, r->verifier);
    if (!e) {
        r->kdf = 1; r->iterations = ACCT_ITERATIONS;
        r->flags = (r->flags | ACCT_PASSWORD_SET) & ~(ACCT_READY_BLANK | ACCT_CHANGE_REQUIRED);
    }
    return e;
}
static void retire_blank(struct acct_db *next, struct acct_record *r)
{
    next->flags |= ACCT_AUTO_RETIRED; next->auto_logon_uid = 0;
    r->flags &= ~ACCT_READY_BLANK;
    if (!(r->flags & ACCT_ENABLED) && !(r->flags & ACCT_PASSWORD_SET)) {
        r->kdf = r->iterations = 0; memset(r->salt,0,16); memset(r->verifier,0,32);
    }
}
static int skip_occupied_roots(struct acct_service *s, const struct acct_request *q)
{
    int skipped=0;
    while(s->store.high_uid!=UINT32_MAX) {
        char root[64]; snprintf(root,sizeof(root),"/Windows/Profiles/u%08x",s->store.high_uid);
        int occupied=s->store.owner.root_occupied(s->store.owner.ctx,s->store.volume,root);
        if(occupied<0) return occupied;
        if(!occupied) return skipped?acct_results_replan(s,q):0;
        if(s->store.high_generation==UINT64_MAX) return -EOVERFLOW;
        uint32_t next=s->store.high_uid==ACCT_LAST_UID?UINT32_MAX:s->store.high_uid+1;
        int e=acct_store_reserve_generation(&s->store,s->db.sid,s->store.high_generation+1,next);
        if(e) { s->available=0; s->faulted=1; return e; }
        skipped=1;
    }
    return -ENOSPC;
}
static int mutate(struct acct_service *s, struct acct_job *job)
{
    struct acct_request *q = &job->request;
    const struct acct_subject *subject = &job->subject;
    struct acct_kernel *k = &s->store.kernel;
    if (q->generation != s->db.generation) return -EAGAIN;
    int is_admin = admin(subject), index = find_uid(&s->db,q->uid);
    int self = subject->uid == q->uid;
    if(q->operation==ACCT_UPDATE && (q->selectors&2048) && self) return -EPERM;
    if (q->operation == ACCT_CREATE) {
        if (!is_admin) return -EPERM;
        if (!q->groups) q->groups=ACCT_STANDARD;
        if (s->db.count==ACCT_MAX || s->store.high_uid==UINT32_MAX) return -ENOSPC;
        if (!acct_name_valid(q->name) || acct_reserved_name(q->name) || find_name(&s->db,q->name)>=0 ||
            (q->groups!=ACCT_ADMIN && q->groups!=ACCT_STANDARD) || (q->flags&~ACCT_ENABLED) ||
            !acct_password_valid(q->password,0) || strcmp(q->password,q->confirm)) return -EINVAL;
    } else {
        if (index<0) return -ENOENT;
        /* sec-k08 (lead decision U4-5): a Standard user or Guest changes
         * only their own password and picture (Windows XP limited accounts);
         * the name, like type and status, is an administrator's change. The
         * service refuses it, whatever the page offers. */
        int personal=q->operation==ACCT_PASSWORD || q->operation==ACCT_PICTURE ||
            (q->operation==ACCT_UPDATE && !(q->selectors&~4096u));
        if (!is_admin && !(self && personal)) return -EPERM;
        int destructive=q->operation==ACCT_DELETE || q->operation==ACCT_ROLE ||
            (q->operation==ACCT_ENABLE && !(q->flags&ACCT_ENABLED)) ||
            (q->operation==ACCT_PASSWORD && !self) ||
            (q->operation==ACCT_UPDATE && ((q->selectors&512) || ((q->selectors&256) && !(q->flags&1)) || (q->selectors&2048)));
        if (destructive) { int active=k->active_uid(k->ctx,q->uid); if(active) return active<0?active:-EBUSY; }
    }
    if(q->operation==ACCT_CREATE) {
        int e=skip_occupied_roots(s,q); if(e) return e;
    }
    if (s->store.high_generation==UINT64_MAX) return -EOVERFLOW;
    struct acct_db *next=k->allocate(k->ctx,sizeof(*next)); if(!next) return -ENOMEM;
    *next=s->db; next->generation=s->store.high_generation+1;
    next->next_uid=s->store.high_uid;
    if(next->next_uid==UINT32_MAX) next->flags|=ACCT_EXHAUSTED;
    struct acct_record *r=index<0?NULL:&next->records[index]; int e=0;
    int was_blank=r && (r->flags&ACCT_READY_BLANK);
    /* Prerequisite applies to the committed input, including compound UPDATE.
     * Password+rename or password+enable is not an implicit bypass. */
    if(was_blank && (q->operation==ACCT_RENAME || q->operation==ACCT_ROLE ||
        (q->operation==ACCT_UPDATE && (q->selectors&(512|1024))))) e=-EPERM;
    switch(e?0:q->operation) {
    case ACCT_CREATE:
        r=&next->records[next->count++]; memset(r,0,sizeof(*r)); r->uid=next->next_uid;
        r->groups=q->groups; r->flags=q->flags; strcpy(r->name,q->name);
        e=acct_set_password(s,r,q->password);
        if(next->next_uid==ACCT_LAST_UID) { next->next_uid=UINT32_MAX; next->flags|=ACCT_EXHAUSTED; }
        else ++next->next_uid;
        break;
    case ACCT_DELETE:
        if(r->flags&ACCT_BUILTIN) { e=-EPERM; break; }
        if(was_blank) { next->flags|=ACCT_AUTO_RETIRED; next->auto_logon_uid=0; }
        memmove(r,r+1,(next->count-(unsigned)index-1)*sizeof(*r)); memset(&next->records[--next->count],0,sizeof(*r));
        break;
    case ACCT_ENABLE:
        if((q->flags&~1u) || (q->flags && (!(r->flags&ACCT_PASSWORD_SET) || (r->flags&ACCT_CHANGE_REQUIRED)))) { e=-EINVAL; break; }
        r->flags=(r->flags&~1u)|q->flags;
        if(was_blank && !q->flags) { r->flags&=~ACCT_PASSWORD_SET; retire_blank(next,r); }
        break;
    case ACCT_ROLE:
        if((r->flags&ACCT_BUILTIN) || (q->groups!=ACCT_ADMIN && q->groups!=ACCT_STANDARD)) { e=-EPERM; break; }
        r->groups=q->groups; break;
    case ACCT_RENAME:
        if(r->flags&ACCT_BUILTIN) { e=-EPERM; break; }
        if(!acct_name_valid(q->name) || acct_reserved_name(q->name) ||
            (find_name(next,q->name)>=0 && !acct_name_equal(r->name,q->name))) { e=-EINVAL; break; }
        memset(r->name,0,32); strcpy(r->name,q->name); break;
    case ACCT_PASSWORD:
        if(!acct_password_valid(q->password,0) || strcmp(q->password,q->confirm)) { e=-EINVAL; break; }
        if(self && !acct_verify(r,q->old_password)) { e=-EACCES; break; }
        e=acct_set_password(s,r,q->password); if(!e && r->uid==1000 && next->origin==ACCT_READY) retire_blank(next,r);
        break;
    case ACCT_PICTURE:
        if(!acct_picture_syntax(q->picture) || !k->picture_valid(k->ctx,r->uid,q->picture)) { e=-EINVAL; break; }
        memset(r->picture,0,96); strcpy(r->picture,q->picture); break;
    case ACCT_UPDATE:
        if(q->selectors&256) {
            if((q->flags&1) && (!(r->flags&ACCT_PASSWORD_SET) || (r->flags&ACCT_CHANGE_REQUIRED))) { e=-EINVAL; break; }
            r->flags=(r->flags&~1u)|(q->flags&1);
            if(was_blank && !(q->flags&1)) { r->flags&=~ACCT_PASSWORD_SET; retire_blank(next,r); }
        }
        if(q->selectors&512) {
            if(r->flags&ACCT_BUILTIN) { e=-EPERM; break; }
            r->groups=(q->flags&2)?ACCT_ADMIN:ACCT_STANDARD;
        }
        if(q->selectors&1024) {
            if((r->flags&ACCT_BUILTIN) || !acct_name_valid(q->name) || acct_reserved_name(q->name) ||
                (find_name(next,q->name)>=0 && !acct_name_equal(r->name,q->name))) { e=-EINVAL; break; }
            memset(r->name,0,32); strcpy(r->name,q->name);
        }
        if(q->selectors&2048) {
            if(!acct_password_valid(q->password,0)) { e=-EINVAL; break; }
            e=acct_set_password(s,r,q->password); if(!e && r->uid==1000 && next->origin==ACCT_READY) retire_blank(next,r);
        }
        if(q->selectors&4096) {
            if(!acct_picture_syntax(q->picture) || !k->picture_valid(k->ctx,r->uid,q->picture)) { e=-EINVAL; break; }
            memcpy(r->picture,q->picture,96);
        }
        break;
    default: if(!e) e=-EINVAL;
    }
    /* Complete-vector validation happens before the first persistent write. */
    if(!e && !has_administrator(next)) e=-EPERM;
    if(!e) e=acct_validate(next,0);
    if(!e && !k->peer_alive(k->ctx,subject)) e=-ESRCH;
    if(!e) {
        s->store.subject=subject;
        e=(q->operation==ACCT_CREATE || q->operation==ACCT_DELETE)?
            acct_store_commit(&s->store,&s->db,next,NULL,0):acct_store_local_commit(&s->store,&s->db,next);
        s->store.subject=NULL;
        if(!e) s->db=*next; else { s->available=0; s->faulted=1; }
    }
    acct_wipe(next,sizeof(*next)); k->deallocate(k->ctx,next,sizeof(*next)); return e;
}
static int profile(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    struct acct_request *q=&job->request; int i=q->uid?find_uid(&s->db,q->uid):find_name(&s->db,q->name);
    int trusted=job->subject.role==ACCT_ROLE_LOGON || job->subject.role==ACCT_ROLE_ACCOUNT;
    /* The sealed SYSTEM broker resolves the private hive of its retained
     * caller. uid 0 by itself remains untrusted, as for all account APIs. */
    if (job->subject.role==ACCT_ROLE_REGISTRY && !job->subject.uid && !job->subject.session_id) trusted=1;
    /* Deny a foreign selector before disclosing whether that mapping exists. */
    if(!admin(&job->subject) && !trusted && (q->uid?q->uid!=job->subject.uid:
        (i<0 || s->db.records[i].uid!=job->subject.uid))) return -EACCES;
    if(i<0) return -ESRCH;
    /* Generation 0 is a pure read of the current mapping (the protocol admits
     * it for LOOKUP and PROFILE; w32srv and libc use it); a stated generation
     * must still be current. */
    if(q->generation && q->generation!=s->db.generation) return -EAGAIN;
    char root[64]={0}; struct acct_owner_binding b={0}; uint64_t generation=0;
    int e=s->store.owner.generation(s->store.owner.ctx,&generation);
    if(!e) e=s->store.owner.binding(s->store.owner.ctx,s->store.volume,s->db.records[i].uid,root,&b);
    if(e==-ENOENT || e==-EACCES) return -EACCES;
    if(e) return e;
    if(!b.key || !b.generation || b.generation>generation || b.uid!=s->db.records[i].uid || b.volume!=s->store.volume ||
        b.action==ACCT_OWNER_RETIRE || !acct_profile_root_valid(root)) return -EIO;
    size_t len=strlen(root); if(len+sizeof("/Documents")>96) return -EOVERFLOW;
    memset(out->body,0,272); w32(out->body,1); w32(out->body+4,272); w32(out->body+8,b.uid);
    w64(out->body+16,s->db.generation); w64(out->body+24,b.generation); memcpy(out->body+32,root,len);
    memcpy(out->body+96,root,len); memcpy(out->body+96+len,"/Documents",11);
    memcpy(out->body+192,"/Programs/terminal",19); out->body_bytes=272; return 0;
}
static int execute(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    struct acct_kernel *k=&s->store.kernel; struct acct_request *q=&job->request;
    if(s->faulted) return -EIO;
    if(!k->peer_alive(k->ctx,&job->subject)) {
        int e=job->subject.service==2?acct_results_cancel(s,job):0;
        return e?e:-ESRCH;
    }
    if(authentication(q->operation)) return acct_auth_execute(s,job,out);
    if(q->operation==ACCT_LIST && !s->available) {
        if(!s->enrollment.generation) return -ENOENT; /* no catalogue yet (ENODATA is outside the ABI errno set) */
        out->count=s->enrollment.draft.count; out->generation=s->enrollment.generation;
        for(unsigned i=0;i<out->count;++i) {
            const struct acct_record *r=&s->enrollment.draft.records[i];
            out->records[i].uid=r->uid; out->records[i].groups=r->groups; out->records[i].flags=r->groups==ACCT_ADMIN?2:0;
            memcpy(out->records[i].name,r->name,32);
        }
        return 0;
    }
    if(!s->available) return -EIO;
    if(q->operation==ACCT_LIST || q->operation==ACCT_LOOKUP) {
        snapshot(&s->db,&job->subject,out);
        if(q->operation==ACCT_LOOKUP) {
            unsigned i=0; while(i<out->count && (q->uid?out->records[i].uid!=q->uid:!acct_name_equal(out->records[i].name,q->name))) ++i;
            if(i==out->count) return -ESRCH;
            out->records[0]=out->records[i]; out->count=1;
        }
        return 0;
    }
    if(q->operation==ACCT_PROFILE) return profile(s,job,out);
    if(q->operation==ACCT_STATUS || q->operation==ACCT_RESERVE) return acct_results_execute(s,job,out);
    int cached=0; int e=acct_results_admit(s,job,out,&cached); if(e || cached) return e;
    e=mutate(s,job);
    return acct_results_finish(s,q,e);
}
/* sec-user-06: the management audit record (adapter.h audit). The new
 * account of a committed CREATE is the database's last record. */
static void audit_management(const struct acct_service *s, int status)
{
    const struct acct_request *q = &s->active.request;
    unsigned op = q->operation;
    if (!s->store.kernel.audit || (op != ACCT_CREATE && op != ACCT_DELETE && op != ACCT_RENAME && op != ACCT_ENABLE &&
                                   op != ACCT_ROLE && op != ACCT_PASSWORD && op != ACCT_PICTURE && op != ACCT_UPDATE)) return;
    uint32_t target = q->uid, groups = q->groups;
    if (op == ACCT_CREATE && !status && s->db.count) target = s->db.records[s->db.count - 1].uid;
    for (uint32_t i = 0; op != ACCT_CREATE && op != ACCT_ROLE && i < s->db.count; i++)
        if (s->db.records[i].uid == target) groups = s->db.records[i].groups;
    const char *type = (groups & 1u) ? "admin" : groups == ACCT_GUEST ? "guest" : groups ? "standard" : "-";
    s->store.kernel.audit(s->store.kernel.ctx, op, s->active.subject.uid, target, type, status);
}
int acct_work(struct acct_service *s, struct acct_response *out)
{
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!try_lock(&s->worker_lock)) return -EBUSY;
    if (!try_lock(&s->queue_lock)) { unlock(&s->worker_lock); return -EAGAIN; }
    if (!s->active.occupied) { unlock(&s->queue_lock); unlock(&s->worker_lock); return 0; }
    uint32_t retry = s->completed ? 0 : retry_ms(s);
    if (!s->faulted && retry && throttled(s->active.request.operation)) {
        out->retry_after_ms = retry;
        unlock(&s->queue_lock); unlock(&s->worker_lock); return -EAGAIN;
    }
    /* Active stays occupied while running, so submit can only fill pending. */
    if (!s->completed) {
        struct acct_job job = s->active;
        unlock(&s->queue_lock);
        memset(&s->completion, 0, sizeof(s->completion));
        s->completion.ticket = job.ticket;
        s->completion.error = execute(s, &job, &s->completion);
        if(s->faulted) acct_attempts_clear(s);
        if (!s->completion.error && !s->completion.generation) s->completion.generation = s->db.generation;
        s->completed = 1;
        acct_wipe(&job, sizeof(job));
        /* Contention never spins or repeats a committed mutation/KDF. A later
         * worker poll publishes the saved completion under the short lock. */
        if (!try_lock(&s->queue_lock)) { unlock(&s->worker_lock); return -EAGAIN; }
    }
    if (!s->store.kernel.peer_alive(s->store.kernel.ctx, &s->active.subject)) {
        memset(&s->completion, 0, sizeof(s->completion));
        s->completion.ticket = s->active.ticket; s->completion.error = -ESRCH;
    }
    *out = s->completion;
    if (out->error == -EACCES && (authentication(s->active.request.operation) || s->active.request.operation == ACCT_PASSWORD)) {
        uint64_t now = s->store.kernel.monotonic_ms(s->store.kernel.ctx);
        s->retry_at = now > UINT64_MAX - 1000 ? UINT64_MAX : now + 1000;
        out->retry_after_ms = 1000;
    }
    struct acct_subject subject = s->active.subject;
    uint32_t wire_operation=s->active.request.wire_operation;
    audit_management(s, out->error);   /* sec-user-06 */
    acct_wipe(&s->active, sizeof(s->active));
    s->active = s->pending;
    acct_wipe(&s->pending, sizeof(s->pending));
    acct_wipe(&s->completion, sizeof(s->completion)); s->completed = 0;
    unlock(&s->queue_lock);
    if(subject.request_id) {
        uint8_t body[16+144*ACCT_MAX]; size_t bytes=0;
        int error=acct_reply_body(subject.service,wire_operation,out,body,sizeof(body),&bytes);
        if(!error) error=s->store.kernel.deliver_reply(s->store.kernel.ctx,&subject,wire_operation,out->generation,out->error,body,bytes);
        if(error) { out->error=error; out->count=0; }
        acct_wipe(body,sizeof(body));
    }
    s->store.kernel.exchange_end(s->store.kernel.ctx,&subject);
    s->store.kernel.release_peer(s->store.kernel.ctx, &subject);
    acct_wipe(&subject, sizeof(subject));
    unlock(&s->worker_lock);
    return 1;
}
