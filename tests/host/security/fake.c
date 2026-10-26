/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
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
    if (n > ACCT_FILE_MAX) return -ENOSPC;
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
    if (endpoint < 0 || endpoint >= 4) return -EBADF;
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
    int yes = n < 4 && f->peers[n].alive && f->peers[n].epoch == s->retained >> 8;
    pthread_mutex_unlock(&f->peer_lock);
    return yes;
}
static void release(void *ctx, struct acct_subject *s)
{
    struct fake *f = ctx;
    unsigned n = (s->retained & 255u) - 1;
    pthread_mutex_lock(&f->peer_lock);
    assert(n < 4 && f->peers[n].refs); --f->peers[n].refs;
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
static int binding(void *ctx, uint64_t volume, uint32_t uid, char root[64], struct acct_owner_reply *r)
{
    struct fake *f = ctx;
    if (f->failed || f->wrong_owner) return -EIO;
    for (unsigned i = f->owner_used; i; --i) {
        struct fake_owner_entry *e = &f->owners[i - 1];
        if (e->request.volume != volume || e->request.uid != uid || e->reply.state != ACCT_OWNER_COMMITTED) continue;
        if (e->request.action == ACCT_OWNER_RETIRE) return -EACCES;
        memcpy(root, e->request.root, 64); *r = e->reply; return 0;
    }
    return -ENOENT;
}
static int owner_call(void *ctx, unsigned op, const struct acct_owner_request *q, struct acct_owner_reply *r)
{
    struct fake *f = ctx;
    if (f->failed) return -EIO;
    struct fake_owner_entry *entry = NULL;
    for (unsigned i = 0; i < f->owner_used; ++i)
        if (f->owners[i].request.transaction == q->transaction) entry = &f->owners[i];
    if (entry && memcmp(&entry->request, q, sizeof(*q))) return -EAGAIN;
    if (op == ACCT_OWNER_STATUS) {
        if (entry) *r = entry->reply;
        else memset(r, 0, sizeof(*r));
        return 0;
    }
    if (op == ACCT_OWNER_PREPARE) {
        ++f->prepare_calls;
        if (!entry) {
            if (q->expected_generation != f->owner_generation) return -EAGAIN;
            if (f->owner_full || f->owner_used == FAKE_OWNERS) return -ENOSPC;
            char root[64]; struct acct_owner_reply found;
            int exists = binding(f, q->volume, q->uid, root, &found);
            if (q->action == ACCT_OWNER_CREATE && exists != -ENOENT) return -EEXIST;
            if (q->action == ACCT_OWNER_RETIRE && exists) return exists;
            entry = &f->owners[f->owner_used++]; entry->request = *q;
            entry->reply.state = ACCT_OWNER_PENDING;
            entry->reply.key = 100 + f->owner_used;
        }
        if (f->cancel_on_prepare) f->peers[1].alive = 0;
    } else if (op == ACCT_OWNER_COMMIT) {
        ++f->commit_calls;
        if (!entry || entry->reply.state == ACCT_OWNER_ABORTED) return -EIO;
        if (entry->reply.state != ACCT_OWNER_COMMITTED) {
            /* Deliberately non-unit increments: service must not guess them. */
            f->owner_generation += 7;
            entry->reply.generation = f->owner_generation;
            entry->reply.state = ACCT_OWNER_COMMITTED;
        }
    } else if (op == ACCT_OWNER_ABORT) {
        ++f->abort_calls;
        if (!entry || entry->reply.state == ACCT_OWNER_COMMITTED) return -EIO;
        entry->reply.state = ACCT_OWNER_ABORTED;
    } else return -EINVAL;
    *r = entry->reply;
    return event(f); /* owner service is independently durable */
}
void fake_init(struct fake *f)
{
    memset(f, 0, sizeof(*f)); f->active_uid = UINT32_MAX; f->now = 100;
    assert(!pthread_mutex_init(&f->peer_lock, NULL));
    for (unsigned i = 0; i < 4; ++i) { f->peers[i].alive = 1; f->peers[i].epoch = 1; }
    f->peers[0].subject.group_count = 1; f->peers[0].subject.trusted_gate = 1;
    f->peers[1].subject.uid = 1000; f->peers[1].subject.group_count = 2;
    f->peers[1].subject.groups[0] = 544; f->peers[1].subject.groups[1] = 545;
    f->peers[2].subject.uid = 1001; f->peers[2].subject.group_count = 1; f->peers[2].subject.groups[0] = 545;
    f->peers[3].subject.uid = 501; f->peers[3].subject.group_count = 1; f->peers[3].subject.groups[0] = 546;
    for (unsigned i = 1; i < 4; ++i) f->peers[i].subject.session_id = 1;
}
void fake_destroy(struct fake *f)
{
    assert(!f->outstanding && !f->deallocated_dirty);
    for (unsigned i = 0; i < 4; ++i) assert(!f->peers[i].refs);
    assert(!pthread_mutex_destroy(&f->peer_lock));
}
struct acct_store fake_store(struct fake *f)
{
    struct acct_store s = {0};
    s.volume = 1234;
    s.io = (struct acct_io){f, 0, read_file, write_file, flush_file, barrier, rename_file, remove_file};
    s.kernel = (struct acct_kernel){f, 0, lease, unlease, retain, alive, release, active,
                                  clock_ms, entropy, allocate, deallocate, picture};
    s.owner = (struct acct_owner){f, 0, owner_auth, generation, binding, owner_call};
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
    memset(d, 0, sizeof(*d)); d->generation = 1; d->next_uid = 1001; d->count = 3;
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
        for (unsigned j = 0; j < 16; ++j) r->salt[j] = (uint8_t)j;
        assert(!acct_derive("password", r->salt, ACCT_ITERATIONS, r->verifier));
    }
}
void fake_boot(struct fake *f, struct acct_service *s)
{
    struct acct_store store = fake_store(f);
    assert(!acct_service_open(s, &store, 1));
    struct acct_db seed;
    fake_seed(&seed, 1);
    uint8_t bytes[ACCT_FILE_MAX]; size_t n;
    assert(!acct_encode(&seed, 1, bytes, sizeof(bytes), &n));
    assert(!acct_service_bootstrap(s, bytes, n, ACCT_READY));
    acct_wipe(&seed, sizeof(seed)); acct_wipe(bytes, sizeof(bytes));
}
void fake_request(struct acct_request *q, unsigned op, uint64_t gen, uint32_t uid, const char *name, const char *password)
{
    memset(q, 0, sizeof(*q)); q->operation = op; q->generation = gen; q->uid = uid;
    if (name) strcpy(q->name, name);
    if (password) { strcpy(q->password, password); strcpy(q->confirm, password); }
}
int fake_run(struct acct_service *s, int endpoint, struct acct_request *q, struct acct_response *r)
{
    int e = acct_submit(s, endpoint, q, sizeof(*q), r);
    assert(!q->password[0] && !q->old_password[0] && !q->confirm[0]);
    if (!e) { assert(acct_work(s, r) == 1); e = r->error; }
    return e;
}
