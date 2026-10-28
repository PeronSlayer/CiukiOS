/* SPDX-License-Identifier: MIT
 * ACCOUNT first-boot adapter: G32 migration evidence and seed promotion
 * (sec-user-02). See firstboot.h for the contract sections it implements.
 */
#include "firstboot.h"
#include "wire.h"
#include "../desktop/users.h"
#include "../../sdk/private/provision.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>

static const char *const legacy_names[3] = { "users.dat", "users.new", "users.lock" };
/* Any of these means v2 provisioning already happened (or was attempted);
 * a G32 file beside them is never a request to (re)import. */
static const char *const v2_evidence[] = {
    "bootstrap.state", "bootstrap.state.new", "bootstrap.state.bak", "bootstrap.state.txn", "bootstrap.state.tmp",
    "accounts.db", "accounts.db.new", "accounts.db.bak", "accounts.db.txn", "accounts.alloc.0", "accounts.alloc.1",
    "accounts.local", "machine.sid", "bootstrap.sid", "provision.seed", "enrollment.source", "enrollment.catalogue",
};

static void hash(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }
static int present(const struct acct_store *s, const char *name)
{
    uint8_t probe[1]; size_t n = 0;
    int e = s->io.read(s->io.ctx, name, probe, sizeof(probe), &n);
    acct_wipe(probe, sizeof(probe));
    return e == -ENOENT ? 0 : 1; /* any other result: present or unreadable, fail closed */
}
static int durable(const struct acct_store *s, const char *name, const void *p, size_t n)
{
    int e = s->io.write(s->io.ctx, name, p, n);
    if (!e) e = s->io.flush(s->io.ctx, name);
    if (!e) e = s->io.barrier(s->io.ctx);
    return e;
}
static void marker_encode(uint8_t m[96], uint32_t files, uint64_t volume, const uint8_t source[32])
{
    memset(m, 0, 96); memcpy(m, "CIUKIMIG", 8); w32(m + 8, 1); w32(m + 12, files);
    w64(m + 16, volume); memcpy(m + 24, source, 32); sha256(m, 64, m + 64);
}
static int marker_decode(const uint8_t *m, size_t n, uint64_t volume, uint32_t *files, uint8_t source[32])
{
    uint8_t sum[32];
    if (n != 96 || memcmp(m, "CIUKIMIG", 8) || a32(m + 8) != 1 || !(a32(m + 12) & 1) || (a32(m + 12) & ~7u) ||
        a64(m + 16) != volume || azero(m + 24, 32) || !azero(m + 56, 8)) return -EIO;
    sha256(m, 64, sum);
    if (!acct_equal_secret(sum, m + 64, 32)) return -EIO;
    *files = a32(m + 12); memcpy(source, m + 24, 32);
    return 0;
}
struct prepare_frame { uint8_t bytes[ACCT_LEGACY_MAX + 1], copy[ACCT_LEGACY_MAX + 1]; struct users_db old; };

static int prepare_locked(const struct acct_store *s, const struct acct_legacy_io *legacy, struct prepare_frame *f)
{
    uint8_t m[97], digest[32], state[CIUKI_PROVISION_STATE_BYTES + 1];
    size_t n = 0, mn = 0;
    int have_marker = present(s, ACCT_MIGRATION_MARKER), have_state = present(s, "bootstrap.state");
    if (!have_marker) {
        if (have_state || !legacy) return ACCT_FIRSTBOOT_NOTHING;
        int e = legacy->read(legacy->ctx, "users.dat", f->bytes, ACCT_LEGACY_MAX + 1, &n);
        if (e == -ENOENT) return ACCT_FIRSTBOOT_NOTHING; /* no G32 store: never a ready-image signal */
        if (e) return e;
        /* Corrupt records/checksums stop conversion and keep the originals. */
        if (!n || n > ACCT_LEGACY_MAX || users_decode(&f->old, f->bytes, n)) return -EIO;
        for (unsigned i = 0; i < sizeof(v2_evidence) / sizeof(*v2_evidence); ++i)
            if (present(s, v2_evidence[i])) return -EIO;
        uint32_t files = 0;
        for (unsigned i = 0; i < 3; ++i) {
            size_t k = 0; char name[96];
            e = legacy->read(legacy->ctx, legacy_names[i], f->copy, ACCT_LEGACY_MAX + 1, &k);
            if (e == -ENOENT) continue;
            if (e) return e;
            if (k > ACCT_LEGACY_MAX || (!i && (k != n || memcmp(f->copy, f->bytes, n)))) return -EIO;
            snprintf(name, sizeof(name), "%s%s", ACCT_MIGRATION_PREFIX, legacy_names[i]);
            e = durable(s, name, f->copy, k);
            if (e) return e;
            files |= 1u << i;
        }
        hash(f->bytes, n, digest);
        marker_encode(m, files, s->volume, digest);
        int e2 = durable(s, ACCT_MIGRATION_MARKER, m, 96);
        if (e2) return e2;
    }
    /* Resume from the protected evidence only; the shared originals may be gone. */
    uint32_t files = 0;
    int e = s->io.read(s->io.ctx, ACCT_MIGRATION_MARKER, m, sizeof(m), &mn);
    if (!e) e = marker_decode(m, mn, s->volume, &files, digest);
    if (!e) e = s->io.read(s->io.ctx, ACCT_MIGRATION_SOURCE, f->bytes, ACCT_LEGACY_MAX + 1, &n);
    if (e) return e == -ENOENT ? -EIO : e;
    uint8_t actual[32]; hash(f->bytes, n, actual);
    if (!n || n > ACCT_LEGACY_MAX || !acct_equal_secret(actual, digest, 32) || users_decode(&f->old, f->bytes, n)) return -EIO;
    int result = ACCT_FIRSTBOOT_PREPARED;
    if (!have_state) {
        if (ciuki_provision_state(state, CIUKI_PROVISION_ENROLLMENT, s->volume, digest, hash)) return -EIO;
        /* A private temporary name: bootstrap recovery treats any torn
         * .new/.bak candidate as contradictory, so never stage under those. */
        e = durable(s, "bootstrap.state.tmp", state, CIUKI_PROVISION_STATE_BYTES);
        if (!e) e = s->io.rename(s->io.ctx, "bootstrap.state.tmp", "bootstrap.state");
        if (!e) e = s->io.barrier(s->io.ctx);
        if (e) return e;
    } else {
        struct acct_bootstrap b;
        e = s->io.read(s->io.ctx, "bootstrap.state", state, sizeof(state), &mn);
        if (!e) e = acct_boot_decode(&b, state, mn);
        if (e || b.origin != ACCT_UPGRADE || memcmp(b.seed_digest, digest, 32) || b.volume != s->volume) return -EIO;
        if (b.state != ACCT_UNPROVISIONED) result = ACCT_FIRSTBOOT_NOTHING;
    }
    /* No verifier copy remains in shared settings (contract 6.3). */
    if (legacy) {
        for (unsigned i = 0; i < 3; ++i) {
            e = legacy->remove(legacy->ctx, legacy_names[i]);
            if (e && e != -ENOENT) return e;
        }
        e = legacy->barrier(legacy->ctx);
        if (e) return e;
    }
    return result;
}

int acct_firstboot_prepare(const struct acct_store *s, const struct acct_legacy_io *legacy)
{
    if (!s || !s->volume || !s->io.read || !s->io.write || !s->io.flush || !s->io.barrier || !s->io.rename ||
        !s->kernel.allocate || !s->kernel.deallocate || !s->kernel.lease_acquire || !s->kernel.lease_release ||
        (legacy && (!legacy->read || !legacy->remove || !legacy->barrier))) return -EINVAL;
    /* Only with a working OWNER backend: the interim image (owner_interim.c,
     * every call ENOSYS) can never enroll, so the G32 files stay where the
     * interim desktop reads them until the real OWNER is bound. */
    uint64_t owner_generation = 0;
    if (!s->owner.generation || s->owner.generation(s->owner.ctx, &owner_generation)) return ACCT_FIRSTBOOT_NOTHING;
    const struct acct_kernel *k = &s->kernel;
    int e = k->lease_acquire(k->ctx);
    if (e) return e;
    struct prepare_frame *f = acct_frame_alloc(k, sizeof(*f));
    e = f ? prepare_locked(s, legacy, f) : -ENOMEM;
    acct_frame_free(k, f, sizeof(*f));
    k->lease_release(k->ctx);
    return e;
}

static void remove_consumed_seed(struct acct_store *s)
{
    /* Recovery may have finished CONSUMED without returning through promote. */
    if (!s->io.remove(s->io.ctx,"provision.seed")) (void)s->io.barrier(s->io.ctx);
}
int acct_firstboot_promote(struct acct_service *s)
{
    if (!s || !s->leased) return -EINVAL;
    if (s->available) { remove_consumed_seed(&s->store); return ACCT_FIRSTBOOT_NOTHING; }
    struct acct_store *st = &s->store;
    struct acct_bootstrap b;
    int e = acct_boot_read(st, &b);
    if (e) return e;
    if (b.state != ACCT_UNPROVISIONED) return ACCT_FIRSTBOOT_NOTHING;
    uint8_t *bytes = acct_frame_alloc(&st->kernel, ACCT_FILE_MAX + 1);
    if (!bytes) return -ENOMEM;
    size_t n = 0;
    if (b.origin == ACCT_UPGRADE) {
        e = st->io.read(st->io.ctx, ACCT_MIGRATION_SOURCE, bytes, ACCT_LEGACY_MAX + 1, &n);
        if (!e && n > ACCT_LEGACY_MAX) e = -EIO;
        if (!e) e = acct_enrollment_source(s, bytes, n);
        acct_frame_free(&st->kernel, bytes, ACCT_FILE_MAX + 1);
        return e ? (e == -ENOENT ? -EIO : e) : ACCT_FIRSTBOOT_ENROLLMENT;
    }
    e = st->io.read(st->io.ctx, "provision.seed", bytes, ACCT_FILE_MAX + 1, &n);
    if (!e && n > ACCT_FILE_MAX) e = -EIO;
    if (!e) e = acct_service_bootstrap(s, bytes, n, b.origin);
    acct_frame_free(&st->kernel, bytes, ACCT_FILE_MAX + 1);
    if (e) return e == -ENOENT ? -EIO : e;
    /* CONSUMED is durable: a residual seed (crash before this unlink) is
     * never replayed, so removal failure does not undo the promotion. */
    remove_consumed_seed(st);
    return ACCT_FIRSTBOOT_PROMOTED;
}
