/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define DB "accounts.db"
#define NEW "accounts.db.new"
#define BAK "accounts.db.bak"
#define TXN "accounts.db.txn"
#define PENDING "provision.pending"
#define USED "provision.used"
#define INTENT_BYTES 224u
#define CANCELLED UINT32_C(0x80000000)

struct workspace {
    uint8_t current[ACCT_FILE_MAX], staged[ACCT_FILE_MAX], intent[INTENT_BYTES];
    struct acct_db decoded;
};
static void put32(uint8_t *p, uint32_t x)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (8 * i)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put64(uint8_t *p, uint64_t x)
{ put32(p, (uint32_t)x); put32(p + 4, (uint32_t)(x >> 32)); }
static uint64_t get64(const uint8_t *p)
{ return get32(p) | (uint64_t)get32(p + 4) << 32; }
static int remove_file(struct acct_store *s, const char *name)
{
    int e = s->io.remove(s->io.ctx, name);
    return e == -ENOENT ? 0 : e;
}
static int durable_write(struct acct_store *s, const char *name, const void *p, size_t n)
{
    int e = s->io.write(s->io.ctx, name, p, n);
    if (!e) e = s->io.flush(s->io.ctx, name);
    if (!e) e = s->io.barrier(s->io.ctx);
    return e;
}
static struct workspace *workspace(struct acct_store *s)
{
    struct workspace *w = s->kernel.allocate(s->kernel.ctx, sizeof(*w));
    if (w) memset(w, 0, sizeof(*w));
    return w;
}
static int finish(struct acct_store *s, struct workspace *w, int e)
{
    acct_wipe(w, sizeof(*w));
    s->kernel.deallocate(s->kernel.ctx, w, sizeof(*w));
    return e;
}
static int read_db(struct acct_store *s, const char *name, uint8_t *b,
                   struct acct_db *d, size_t *n)
{
    int e = s->io.read(s->io.ctx, name, b, ACCT_FILE_MAX, n);
    if (!e) e = acct_decode(d, 0, b, *n);
    return e;
}
static int marker(struct acct_store *s, const struct acct_db *d, int create)
{
    uint8_t expected[64] = {0}, found[65];
    memcpy(expected, "CIUKIUSE", 8);
    for (unsigned i = 0; i < 3; ++i) put32(expected + 8 + 4 * i, d->sid[i]);
    put32(expected + 20, d->origin);
    sha256(expected, 32, expected + 32);
    size_t n = 0;
    int e = s->io.read(s->io.ctx, USED, found, sizeof(found), &n);
    if (e == -ENOENT && create) e = durable_write(s, USED, expected, sizeof(expected));
    else if (e == -ENOENT) e = -EIO;
    else if (!e && (n != 64 || memcmp(expected, found, 64))) e = -EIO;
    acct_wipe(found, sizeof(found));
    acct_wipe(expected, sizeof(expected));
    return e;
}
static int cleanup(struct acct_store *s)
{
    /* Backup must disappear durably BEFORE the intent. A subsequent loss of the
     * canonical file cannot revive an acknowledged older password. */
    int e = remove_file(s, BAK);
    if (!e) e = remove_file(s, NEW);
    if (!e) e = remove_file(s, PENDING);
    if (!e) e = s->io.barrier(s->io.ctx);
    if (!e) e = remove_file(s, TXN);
    if (!e) e = s->io.barrier(s->io.ctx);
    return e;
}
static int owner_request_valid(const struct acct_owner_request *q)
{
    if (q->uid != 500 && q->uid != 501 && (q->uid < 1000 || q->uid == UINT32_MAX)) return 0;
    return q->transaction && q->volume &&
           (q->action == ACCT_OWNER_CREATE || q->action == ACCT_OWNER_RETIRE) &&
           acct_profile_root_valid(q->root);
}
static int cancelled(struct acct_store *s)
{ return s->subject && !s->kernel.peer_alive(s->kernel.ctx, s->subject); }
static int install(struct acct_store *, struct workspace *);
static int cancel_intent(struct acct_store *s, struct workspace *w)
{
    size_t n = 0;
    int e = read_db(s, DB, w->current, &w->decoded, &n);
    if (e || w->decoded.generation != get64(w->intent + 8) ||
        memcmp(w->current + 64, w->intent + 32, 32)) return -EIO;
    /* Persist cancellation before replacing the intended candidate. Recovery
     * can then reconstruct the reservation from the valid old generation. */
    put32(w->intent + 28, get32(w->intent + 28) | CANCELLED);
    memset(w->intent + 96, 0, 32);
    sha256(w->intent, sizeof(w->intent), w->intent + 96);
    e = durable_write(s, TXN, w->intent, sizeof(w->intent));
    if (e) return e;
    /* Consume the cancelled transaction's generation as well as its reserved
     * uid. Never reuse an ABORTED owner transaction id for another operation. */
    w->decoded.generation = get64(w->intent + 16);
    e = acct_encode(&w->decoded, 0, w->staged, sizeof(w->staged), &n);
    if (!e) e = durable_write(s, NEW, w->staged, n);
    put32(w->intent + 24, (uint32_t)n); put32(w->intent + 28, 0);
    memcpy(w->intent + 64, w->staged + 64, 32);
    memset(w->intent + 96, 0, 128);
    sha256(w->intent, sizeof(w->intent), w->intent + 96);
    if (!e) e = durable_write(s, TXN, w->intent, sizeof(w->intent));
    if (!e) {
        const struct acct_subject *subject = s->subject;
        s->subject = NULL;
        e = install(s, w);
        s->subject = subject;
    }
    return e ? e : -ESRCH;
}
static int owners(struct acct_store *s, struct workspace *w)
{
    const uint8_t *intent = w->intent;
    if (!get32(intent + 28)) return 0;
    const uint8_t *p = intent + 128;
    struct acct_owner_request q = {0};
    q.transaction = get64(p); q.expected_generation = get64(p + 8);
    q.volume = get64(p + 16); q.uid = get32(p + 24); q.action = get32(p + 28);
    memcpy(q.root, p + 32, 64);
    if (q.transaction != get64(intent + 16) || q.volume != s->volume || !owner_request_valid(&q)) return -EIO;
    struct acct_owner_reply r = {0};
    int e = s->owner.call(s->owner.ctx, ACCT_OWNER_STATUS, &q, &r);
    if (e) return e;
    if (r.state == ACCT_OWNER_NONE) {
        e = s->owner.call(s->owner.ctx, ACCT_OWNER_PREPARE, &q, &r);
        if (e) return e == -ENOENT ? -EIO : e;
    }
    if (r.state == ACCT_OWNER_PENDING) {
        if (cancelled(s)) {
            e = s->owner.call(s->owner.ctx, ACCT_OWNER_ABORT, &q, &r);
            return e ? e : cancel_intent(s, w);
        }
        e = s->owner.call(s->owner.ctx, ACCT_OWNER_COMMIT, &q, &r);
        if (e) return e;
    }
    if (r.state == ACCT_OWNER_ABORTED) return cancel_intent(s, w);
    return r.state == ACCT_OWNER_COMMITTED && r.key && r.generation > q.expected_generation ? 0 : -EIO;
}
static int bindings(struct acct_store *s, const struct acct_db *d)
{
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        if (!(r->flags & ACCT_ENABLED)) continue;
        char root[64] = {0};
        snprintf(root, sizeof(root), "/Windows/Profiles/u%08x", r->uid);
        struct acct_owner_reply binding = {0};
        int e = s->owner.binding(s->owner.ctx, s->volume, r->uid, root, &binding);
        if (e) return e == -ENOENT ? -EIO : e;
        if (binding.state != ACCT_OWNER_COMMITTED || !binding.key || !binding.generation ||
            !acct_profile_root_valid(root)) return -EIO;
    }
    return 0;
}
/* The seed has exactly one ordinary account. Multi-profile G32 enrollment is
 * intentionally staged by user-02, one reserved uid/owner transaction at a time.
 */
int acct_owner_requests(struct acct_store *s, const struct acct_db *d,
                        struct acct_owner_request *q, unsigned *count)
{
    *count = 0;
    for (unsigned i = 0; i < d->count; ++i) {
        const struct acct_record *r = &d->records[i];
        if (!(r->flags & ACCT_ENABLED)) continue;
        char root[64] = {0};
        snprintf(root, sizeof(root), "/Windows/Profiles/u%08x", r->uid);
        struct acct_owner_reply binding = {0};
        int e = s->owner.binding(s->owner.ctx, s->volume, r->uid, root, &binding);
        if (!e) {
            if (binding.state != ACCT_OWNER_COMMITTED || !binding.key || !binding.generation ||
                !acct_profile_root_valid(root)) return -EIO;
            continue;
        }
        if (e != -ENOENT) return e;
        if (*count) return -EOPNOTSUPP;
        memset(q, 0, sizeof(*q));
        q->transaction = d->generation; q->uid = r->uid;
        q->volume = s->volume; q->action = ACCT_OWNER_CREATE;
        memcpy(q->root, root, 64);
        e = s->owner.generation(s->owner.ctx, &q->expected_generation);
        if (e) return e;
        *count = 1;
    }
    return 0;
}
static int install(struct acct_store *s, struct workspace *w)
{
    if (!get32(w->intent + 28) && cancelled(s)) return cancel_intent(s, w);
    int e = owners(s, w);
    if (e) return e;
    size_t n = 0;
    e = read_db(s, DB, w->current, &w->decoded, &n);
    if (!e && w->decoded.generation == get64(w->intent + 16)) {
        if (n != get32(w->intent + 24) || memcmp(w->current + 64, w->intent + 64, 32)) return -EIO;
    } else {
        /* Do not hide a corrupt or contradictory canonical database. */
        if (e != 0 && e != -ENOENT) return e;
        if (!e && (w->decoded.generation != get64(w->intent + 8) ||
                   memcmp(w->current + 64, w->intent + 32, 32))) return -EIO;
        if (!e) {
            e = s->io.rename(s->io.ctx, DB, BAK);
            if (e) return e;
            e = s->io.barrier(s->io.ctx);
            if (e) return e;
        } else if (get64(w->intent + 8)) {
            e = read_db(s, BAK, w->current, &w->decoded, &n);
            if (e || w->decoded.generation != get64(w->intent + 8) ||
                memcmp(w->current + 64, w->intent + 32, 32)) return -EIO;
        }
        e = s->io.rename(s->io.ctx, NEW, DB);
        if (e) return e;
    }
    e = s->io.flush(s->io.ctx, DB);
    if (!e) e = s->io.barrier(s->io.ctx);
    if (!e) e = read_db(s, DB, w->current, &w->decoded, &n);
    if (!e) e = bindings(s, &w->decoded);
    if (!e) e = marker(s, &w->decoded, get64(w->intent + 8) == 0);
    if (!e) e = cleanup(s);
    return e;
}
int acct_store_commit(struct acct_store *s, const struct acct_db *old,
                      const struct acct_db *next, const struct acct_owner_request *q, unsigned count)
{
    if (count > 1 || (count && (!q || !owner_request_valid(q) || q->transaction != next->generation ||
                              q->volume != s->volume)) || !next->generation ||
        (old && (old->generation == UINT64_MAX || next->generation != old->generation + 1 ||
                 next->next_uid < old->next_uid || memcmp(old->sid, next->sid, sizeof(old->sid)) ||
                 old->origin != next->origin))) return -EINVAL;
    struct workspace *w = workspace(s);
    if (!w) return -ENOMEM;
    size_t n = 0;
    int e = acct_encode(next, 0, w->staged, sizeof(w->staged), &n);
    if (e) return finish(s, w, e);
    size_t found = 0;
    e = s->io.read(s->io.ctx, TXN, w->intent, sizeof(w->intent), &found);
    if (e != -ENOENT) return finish(s, w, e ? e : -EBUSY);
    e = read_db(s, DB, w->current, &w->decoded, &found);
    if (old) {
        uint8_t expected[ACCT_FILE_MAX]; size_t bytes = 0;
        int encoded = acct_encode(old, 0, expected, sizeof(expected), &bytes);
        if (!e && (encoded || bytes != found || memcmp(expected, w->current, found))) e = -EAGAIN;
        acct_wipe(expected, sizeof(expected));
    } else if (e == -ENOENT) e = 0;
    else if (!e) e = -EEXIST;
    if (e) return finish(s, w, e);
    memset(w->intent, 0, sizeof(w->intent));
    memcpy(w->intent, "CIACTXN2", 8);
    put64(w->intent + 8, old ? old->generation : 0);
    put64(w->intent + 16, next->generation);
    put32(w->intent + 24, (uint32_t)n); put32(w->intent + 28, count);
    if (old) memcpy(w->intent + 32, w->current + 64, 32);
    memcpy(w->intent + 64, w->staged + 64, 32);
    if (count) {
        uint8_t *p = w->intent + 128;
        put64(p, q->transaction); put64(p + 8, q->expected_generation);
        put64(p + 16, q->volume); put32(p + 24, q->uid); put32(p + 28, q->action);
        memcpy(p + 32, q->root, 64);
    }
    sha256(w->intent, sizeof(w->intent), w->intent + 96);
    e = durable_write(s, NEW, w->staged, n);
    if (!e) e = durable_write(s, TXN, w->intent, sizeof(w->intent));
    if (!e) e = install(s, w);
    return finish(s, w, e);
}
int acct_store_bootstrap(struct acct_store *s, struct acct_db *d)
{
    struct workspace *w = workspace(s);
    if (!w) return -ENOMEM;
    size_t n = 0;
    int e = s->io.read(s->io.ctx, USED, w->current, sizeof(w->current), &n);
    if (e != -ENOENT) return finish(s, w, e ? e : -EIO);
    e = read_db(s, PENDING, w->current, &w->decoded, &n);
    if (e == -ENOENT && d) {
        uint64_t owner_generation = 0;
        e = s->owner.generation(s->owner.ctx, &owner_generation);
        if (!e && owner_generation) e = -EIO;
        if (e) return finish(s, w, e);
        e = acct_encode(d, 0, w->current, sizeof(w->current), &n);
        if (!e) e = durable_write(s, PENDING, w->current, n);
        if (!e) w->decoded = *d;
    }
    if (e) return finish(s, w, e);
    struct acct_owner_request request;
    unsigned count = 0;
    e = acct_owner_requests(s, &w->decoded, &request, &count);
    if (!e) e = acct_store_commit(s, NULL, &w->decoded, &request, count);
    if (!e && d) *d = w->decoded;
    return finish(s, w, e);
}
int acct_store_recover(struct acct_store *s, struct acct_db *d)
{
    struct workspace *w = workspace(s);
    if (!w) return -ENOMEM;
    size_t n = 0;
    int e = s->io.read(s->io.ctx, TXN, w->intent, sizeof(w->intent), &n);
    if (!e) {
        uint8_t expected[32];
        if (n != INTENT_BYTES || memcmp(w->intent, "CIACTXN2", 8) ||
            !get64(w->intent + 16) || (get32(w->intent + 28) & ~CANCELLED) > 1 ||
            get64(w->intent + 8) >= get64(w->intent + 16)) return finish(s, w, -EIO);
        memcpy(expected, w->intent + 96, 32);
        memset(w->intent + 96, 0, 32);
        sha256(w->intent, sizeof(w->intent), w->intent + 96);
        if (!acct_equal_secret(expected, w->intent + 96, 32)) return finish(s, w, -EIO);
        int cancel = !!(get32(w->intent + 28) & CANCELLED);
        if (!cancel && get32(w->intent + 28)) {
            const uint8_t *p = w->intent + 128;
            struct acct_owner_request request = {0}; struct acct_owner_reply reply = {0};
            request.transaction = get64(p); request.expected_generation = get64(p + 8);
            request.volume = get64(p + 16); request.uid = get32(p + 24); request.action = get32(p + 28);
            memcpy(request.root, p + 32, 64);
            if (!owner_request_valid(&request) || request.transaction != get64(w->intent + 16) ||
                request.volume != s->volume) return finish(s, w, -EIO);
            e = s->owner.call(s->owner.ctx, ACCT_OWNER_STATUS, &request, &reply);
            if (e) return finish(s, w, e);
            cancel = reply.state == ACCT_OWNER_ABORTED;
        }
        if (cancel) e = cancel_intent(s, w);
        else {
            e = read_db(s, DB, w->current, &w->decoded, &n);
            if (e || w->decoded.generation != get64(w->intent + 16))
                e = read_db(s, NEW, w->current, &w->decoded, &n);
            if (e || w->decoded.generation != get64(w->intent + 16) ||
                n != get32(w->intent + 24) || memcmp(w->current + 64, w->intent + 64, 32))
                return finish(s, w, -EIO);
            e = install(s, w);
        }
        if (e && e != -ESRCH) return finish(s, w, e);
    } else if (e != -ENOENT) return finish(s, w, e);
    e = read_db(s, DB, w->current, &w->decoded, &n);
    if (e == -ENOENT) {
        size_t used_n = 0;
        int used = s->io.read(s->io.ctx, USED, w->current, sizeof(w->current), &used_n);
        if (used != -ENOENT) return finish(s, w, -EIO);
        size_t backup_n = 0;
        int backup = s->io.read(s->io.ctx, BAK, w->current, sizeof(w->current), &backup_n);
        if (backup != -ENOENT) return finish(s, w, -EIO);
        e = acct_store_bootstrap(s, NULL);
        if (e == -ENOENT) {
            size_t orphan_n = 0;
            int orphan = s->io.read(s->io.ctx, NEW, w->current, sizeof(w->current), &orphan_n);
            if (orphan != -ENOENT) return finish(s, w, -EIO);
            uint64_t owner_generation = 0;
            int oe = s->owner.generation(s->owner.ctx, &owner_generation);
            if (oe || owner_generation) e = oe ? oe : -EIO;
        }
        if (e) return finish(s, w, e);
        e = read_db(s, DB, w->current, &w->decoded, &n);
    }
    if (!e) e = marker(s, &w->decoded, 0);
    if (!e) e = bindings(s, &w->decoded);
    if (!e) {
        size_t bak_n = 0;
        int backup = s->io.read(s->io.ctx, BAK, w->staged, sizeof(w->staged), &bak_n);
        if (backup != -ENOENT) e = -EIO; /* no intent authorizes this backup */
    }
    if (!e) e = remove_file(s, NEW); /* uncommitted partial write */
    if (!e) e = s->io.barrier(s->io.ctx);
    if (!e) *d = w->decoded;
    return finish(s, w, e);
}
