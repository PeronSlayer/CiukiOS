/* SPDX-License-Identifier: MIT */
#include "accounts.h"
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
        k->peer_alive && k->release_peer && k->active_uid && k->monotonic_ms &&
        k->entropy && k->allocate && k->deallocate && k->picture_valid &&
        s->owner.authenticate && s->owner.generation && s->owner.binding && s->owner.call &&
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
    e = store->owner.authenticate(store->owner.ctx);
    if (!e) e = acct_store_recover(&s->store, &s->db);
    if (!e && !pictures_valid(&store->kernel, &s->db)) e = -EIO;
    if (!e) s->available = 1;
    else if (e != -ENOENT) { acct_service_close(s); return e; }
    s->next_ticket = 1;
    return 0;
}
void acct_service_close(struct acct_service *s)
{
    if (!s) return;
    struct acct_kernel *k = &s->store.kernel;
    if (s->active.occupied) k->release_peer(k->ctx, &s->active.subject);
    if (s->pending.occupied) k->release_peer(k->ctx, &s->pending.subject);
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
    if (e) { acct_wipe(&decoded, sizeof(decoded)); unlock(&s->worker_lock); return e; }
    struct acct_kernel *k = &s->store.kernel;
    struct acct_db *d = k->allocate(k->ctx, sizeof(*d));
    if (d) *d = decoded;
    else e = -ENOMEM;
    acct_wipe(&decoded, sizeof(decoded));
    if (!e) {
        uint8_t sid[12];
        e = k->entropy(k->ctx, sid, sizeof(sid));
        if (!e) {
            for (unsigned i = 0; i < 3; ++i) {
                const uint8_t *p = sid + 4 * i;
                d->sid[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            }
            if (!(d->sid[0] | d->sid[1] | d->sid[2])) e = -EAGAIN;
        }
        acct_wipe(sid, sizeof(sid));
    }
    if (!e && d->origin == ACCT_READY) {
        struct acct_record *r = &d->records[2];
        e = k->entropy(k->ctx, r->salt, 16);
        if (!e) e = acct_derive("", r->salt, ACCT_ITERATIONS, r->verifier);
        if (!e) { r->kdf = 1; r->iterations = ACCT_ITERATIONS; r->flags |= ACCT_PASSWORD_SET; }
    }
    if (!e) e = acct_store_bootstrap(&s->store, d);
    if (!e) { s->db = *d; s->available = 1; }
    if (d) { acct_wipe(d, sizeof(*d)); k->deallocate(k->ctx, d, sizeof(*d)); }
    unlock(&s->worker_lock);
    return e;
}
static int admin(const struct acct_subject *subject)
{
    if (subject->uid == 0 && !subject->session_id) return 1;
    for (unsigned i = 0; i < subject->group_count && i < 8; ++i)
        if (subject->groups[i] == 544) return 1;
    return 0;
}
static int authentication(unsigned op)
{ return op == ACCT_AUTH || op == ACCT_UNLOCK || op == ACCT_ENROLL_VERIFY; }
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
    if (!s->leased || !s->available) e = -EIO;
    else if (q->operation < ACCT_LIST || q->operation > ACCT_ENROLL_VERIFY ||
             !memchr(q->name, 0, 32) || !memchr(q->picture, 0, 96) ||
             !acct_password_valid(q->password, 1) || !acct_password_valid(q->confirm, 1) ||
             !acct_password_valid(q->old_password, 1)) e = -EINVAL;
    struct acct_subject subject = {0};
    struct acct_kernel *k = &s->store.kernel;
    if (!e) e = k->retain_peer(k->ctx, endpoint, &subject);
    int retained = !e;
    if (!e && (!subject.retained || !subject.group_count || subject.group_count > 8)) e = -EACCES;
    if (!e && authentication(q->operation) &&
        (!subject.trusted_gate || subject.uid || subject.session_id)) e = -EPERM;
    if (!e) {
        if (!try_lock(&s->queue_lock)) e = -EAGAIN;
        else {
            uint32_t retry = retry_ms(s);
            if ((authentication(q->operation) || q->operation == ACCT_PASSWORD) && retry) {
                out->retry_after_ms = retry; e = -EAGAIN;
            } else if (!s->next_ticket) e = -EAGAIN;
            else {
                struct acct_job *job = !s->active.occupied ? &s->active :
                    authentication(q->operation) && subject.trusted_gate && !s->pending.occupied ? &s->pending : NULL;
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
    int management = admin(subject) && !subject->trusted_gate;
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        if (!(r->flags & ACCT_ENABLED) && !management &&
            !(subject->trusted_gate && subject->safe_mode && r->uid == 500 && (r->flags & ACCT_PASSWORD_SET))) continue;
        struct acct_public *p = &out->records[out->count++];
        p->uid = r->uid; p->flags = r->flags; p->groups = r->groups;
        memcpy(p->name, r->name, 32); memcpy(p->picture, r->picture, 96);
    }
}
static int verify(const struct acct_record *r, const char *password)
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
static int set_password(struct acct_service *s, struct acct_record *r, const char *password)
{
    int e = s->store.kernel.entropy(s->store.kernel.ctx, r->salt, 16);
    if (!e) e = acct_derive(password, r->salt, ACCT_ITERATIONS, r->verifier);
    if (!e) {
        r->kdf = 1; r->iterations = ACCT_ITERATIONS;
        r->flags = (r->flags | ACCT_PASSWORD_SET) & ~(ACCT_READY_BLANK | ACCT_CHANGE_REQUIRED);
    }
    return e;
}
static int mutate(struct acct_service *s, struct acct_job *job)
{
    struct acct_request *q = &job->request;
    const struct acct_subject *subject = &job->subject;
    struct acct_kernel *k = &s->store.kernel;
    if (q->generation != s->db.generation) return -EAGAIN;
    int is_admin = admin(subject), index = find_uid(&s->db, q->uid);
    if (q->operation == ACCT_CREATE) {
        if (!is_admin) return -EPERM;
        if (!q->groups) q->groups = ACCT_STANDARD;
        if (s->db.count == ACCT_MAX || (s->db.flags & ACCT_EXHAUSTED)) return -ENOSPC;
        if (!acct_name_valid(q->name) || acct_reserved_name(q->name) ||
            (q->groups != ACCT_ADMIN && q->groups != ACCT_STANDARD) || (q->flags & ~ACCT_ENABLED) ||
            !acct_password_valid(q->password, 0) || strcmp(q->password, q->confirm)) return -EINVAL;
        if (find_name(&s->db, q->name) >= 0) return -EEXIST;
    } else {
        if (index < 0) return -ENOENT;
        if (q->operation == ACCT_PASSWORD || q->operation == ACCT_PICTURE) {
            if (subject->uid != q->uid && !is_admin) return -EPERM;
        } else if (!is_admin) return -EPERM;
        if (q->operation == ACCT_DELETE || q->operation == ACCT_ROLE ||
            (q->operation == ACCT_ENABLE && !(q->flags & ACCT_ENABLED)) ||
            (q->operation == ACCT_PASSWORD && subject->uid != q->uid)) {
            int active = k->active_uid(k->ctx, q->uid);
            if (active) return active < 0 ? active : -EBUSY;
        }
    }
    if (s->db.generation == UINT64_MAX || (q->operation == ACCT_CREATE && s->db.generation == UINT64_MAX - 1)) return -EOVERFLOW;
    struct acct_db *next = k->allocate(k->ctx, sizeof(*next));
    if (!next) return -ENOMEM;
    *next = s->db;
    struct acct_record *r = index < 0 ? NULL : &next->records[index];
    int e = 0;
    struct acct_owner_request owner = {0};
    unsigned owner_count = 0;
    switch (q->operation) {
    case ACCT_CREATE:
        r = &next->records[next->count++]; memset(r, 0, sizeof(*r));
        r->uid = next->next_uid; r->groups = q->groups; r->flags = q->flags;
        strcpy(r->name, q->name);
        e = set_password(s, r, q->password);
        if (!e) {
            struct acct_db reservation = s->db;
            ++reservation.generation;
            if (reservation.next_uid == ACCT_LAST_UID) { reservation.next_uid = UINT32_MAX; reservation.flags |= ACCT_EXHAUSTED; }
            else ++reservation.next_uid;
            if (!k->peer_alive(k->ctx, subject)) e = -ESRCH;
            if (!e) e = acct_store_commit(&s->store, &s->db, &reservation, NULL, 0);
            if (!e) { s->db = reservation; next->next_uid = reservation.next_uid; next->flags = reservation.flags; }
            acct_wipe(&reservation, sizeof(reservation));
            if (e && e != -ESRCH) s->available = 0;
        }
        owner.action = ACCT_OWNER_CREATE;
        break;
    case ACCT_DELETE:
        if (r->flags & ACCT_BUILTIN) { e = -EPERM; break; }
        owner.action = ACCT_OWNER_RETIRE;
        memmove(r, r + 1, (next->count - (unsigned)index - 1) * sizeof(*r));
        acct_wipe(&next->records[--next->count], sizeof(*r));
        break;
    case ACCT_RENAME:
        if ((r->flags & ACCT_BUILTIN) || (r->flags & ACCT_READY_BLANK)) { e = -EPERM; break; }
        if (!acct_name_valid(q->name) || acct_reserved_name(q->name)) { e = -EINVAL; break; }
        { int other = find_name(next, q->name);
          if (other >= 0 && other != index) { e = -EEXIST; break; } }
        memset(r->name, 0, 32); strcpy(r->name, q->name);
        break;
    case ACCT_ENABLE:
        if ((q->flags & ~ACCT_ENABLED) || (q->flags && (!(r->flags & ACCT_PASSWORD_SET) ||
                                                      (r->flags & ACCT_CHANGE_REQUIRED)))) { e = -EINVAL; break; }
        r->flags = (r->flags & ~ACCT_ENABLED) | q->flags;
        if (!q->flags && r->uid == 1000) next->auto_logon_uid = 0;
        break;
    case ACCT_ROLE:
        if (r->flags & ACCT_BUILTIN) { e = -EPERM; break; }
        if (q->groups != ACCT_ADMIN && q->groups != ACCT_STANDARD) { e = -EINVAL; break; }
        r->groups = q->groups;
        break;
    case ACCT_PASSWORD:
        if (!acct_password_valid(q->password, 0) || strcmp(q->password, q->confirm)) { e = -EINVAL; break; }
        if (subject->uid == q->uid && !verify(r, q->old_password)) { e = -EACCES; break; }
        e = set_password(s, r, q->password);
        if (!e && r->uid == 1000) next->auto_logon_uid = 0;
        break;
    case ACCT_PICTURE:
        if (!acct_picture_syntax(q->picture) || !k->picture_valid(k->ctx, r->uid, q->picture)) { e = -EINVAL; break; }
        memset(r->picture, 0, 96); strcpy(r->picture, q->picture);
        break;
    default: e = -EINVAL;
    }
    if (!e && !has_administrator(next)) e = -EPERM;
    next->generation = s->db.generation + 1;
    if (!e) e = acct_validate(next, 0);
    if (!e && owner.action) {
        owner_count = 1; owner.uid = q->operation == ACCT_CREATE ? r->uid : q->uid;
        owner.transaction = next->generation; owner.volume = s->store.volume;
        snprintf(owner.root, sizeof(owner.root), "/Windows/Profiles/u%08x", owner.uid);
        if (owner.action == ACCT_OWNER_RETIRE) {
            struct acct_owner_reply binding;
            e = s->store.owner.binding(s->store.owner.ctx, owner.volume, owner.uid, owner.root, &binding);
            if (!e && (binding.state != ACCT_OWNER_COMMITTED || !acct_profile_root_valid(owner.root))) e = -EIO;
        }
        if (!e) e = s->store.owner.generation(s->store.owner.ctx, &owner.expected_generation);
    } else if (!e) e = acct_owner_requests(&s->store, next, &owner, &owner_count);
    if (!e && !k->peer_alive(k->ctx, subject)) e = -ESRCH;
    if (!e) {
        s->store.subject = subject;
        e = acct_store_commit(&s->store, &s->db, next, &owner, owner_count);
        s->store.subject = NULL;
        if (!e) s->db = *next;
        else s->available = 0; /* recover before accepting any further work */
    }
    acct_wipe(next, sizeof(*next)); k->deallocate(k->ctx, next, sizeof(*next));
    return e;
}
static int authentication_owner(struct acct_service *s, struct acct_job *job, const struct acct_record *r)
{
    char root[64] = {0};
    struct acct_owner_reply binding = {0};
    int e = s->store.owner.binding(s->store.owner.ctx, s->store.volume, r->uid, root, &binding);
    if (!e) return binding.state == ACCT_OWNER_COMMITTED && binding.key && binding.generation &&
                   acct_profile_root_valid(root) ? 0 : -EIO;
    /* Disabled recovery account has no profile until its first authenticated
     * Safe Mode use. It must have a committed owner before session admission. */
    if (e != -ENOENT || r->uid != 500 || !job->subject.safe_mode) return e == -ENOENT ? -EIO : e;
    if (s->db.generation == UINT64_MAX) return -EOVERFLOW;
    struct acct_kernel *k = &s->store.kernel;
    struct acct_db *next = k->allocate(k->ctx, sizeof(*next));
    if (!next) return -ENOMEM;
    *next = s->db; ++next->generation;
    struct acct_owner_request owner = {0};
    owner.transaction = next->generation; owner.volume = s->store.volume;
    owner.uid = 500; owner.action = ACCT_OWNER_CREATE;
    snprintf(owner.root, sizeof(owner.root), "/Windows/Profiles/u%08x", owner.uid);
    e = s->store.owner.generation(s->store.owner.ctx, &owner.expected_generation);
    if (!e) {
        s->store.subject = &job->subject;
        e = acct_store_commit(&s->store, &s->db, next, &owner, 1);
        s->store.subject = NULL;
        if (!e) s->db = *next;
        else s->available = 0;
    }
    acct_wipe(next, sizeof(*next)); k->deallocate(k->ctx, next, sizeof(*next));
    return e;
}
static int execute(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    struct acct_kernel *k = &s->store.kernel;
    struct acct_request *q = &job->request;
    if (!s->available) return -EIO;
    if (!k->peer_alive(k->ctx, &job->subject)) return -ESRCH;
    if (q->operation == ACCT_LIST) { snapshot(&s->db, &job->subject, out); return 0; }
    if (!authentication(q->operation)) return mutate(s, job);
    if (q->generation != s->db.generation) return -EAGAIN;
    int i = find_name(&s->db, q->name);
    const struct acct_record *r = i < 0 ? NULL : &s->db.records[i];
    int eligible = r && ((r->flags & ACCT_ENABLED) ||
        (r->uid == 500 && job->subject.safe_mode && (r->flags & ACCT_PASSWORD_SET)));
    int match = verify(eligible ? r : NULL, q->password);
    if (!k->peer_alive(k->ctx, &job->subject)) return -ESRCH;
    if (!eligible || !match) return -EACCES;
    if (r->flags & ACCT_CHANGE_REQUIRED) {
        if (q->operation != ACCT_ENROLL_VERIFY) return -EACCES;
        out->change_required = strlen(q->password) < 8; /* enrollment proof only; no logon uid */
        return 0;
    }
    uint32_t uid = r->uid;
    int e = authentication_owner(s, job, r);
    if (!e) out->authenticated_uid = uid;
    return e;
}
int acct_work(struct acct_service *s, struct acct_response *out)
{
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!try_lock(&s->worker_lock)) return -EBUSY;
    if (!try_lock(&s->queue_lock)) { unlock(&s->worker_lock); return -EAGAIN; }
    if (!s->active.occupied) { unlock(&s->queue_lock); unlock(&s->worker_lock); return 0; }
    uint32_t retry = s->completed ? 0 : retry_ms(s);
    if (retry && (authentication(s->active.request.operation) || s->active.request.operation == ACCT_PASSWORD)) {
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
        if (!s->completion.error) s->completion.generation = s->db.generation;
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
    acct_wipe(&s->active, sizeof(s->active));
    s->active = s->pending;
    acct_wipe(&s->pending, sizeof(s->pending));
    acct_wipe(&s->completion, sizeof(s->completion)); s->completed = 0;
    unlock(&s->queue_lock);
    s->store.kernel.release_peer(s->store.kernel.ctx, &subject);
    acct_wipe(&subject, sizeof(subject));
    unlock(&s->worker_lock);
    return 1;
}
