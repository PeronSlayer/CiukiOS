/* SPDX-License-Identifier: MIT
 * Setup account provisioning and the confirmed lost-password CD repair
 * (sec-user-02; docs/design/user-security-contract.md 3.3-3.6, 5.1-5.3).
 *
 * Seed: the trusted boot-only Setup encodes the exact section 3.6 Setup
 * recipe with the account service's own codec (apps/accounts/codec.c):
 * uid 500 CiukiAdministrator disabled with the chosen recovery password,
 * uid 501 Guest disabled and unset, uid 1000 the custom administrator with
 * its own password. Salts come from kernel entropy; plaintext never leaves
 * the caller's buffers, which the caller wipes. No CiukiUser is created.
 *
 * Repair: on an identified installation whose account database is intact,
 * set a new CiukiAdministrator (uid 500) password. The repair never writes
 * accounts.db itself: it reserves two fresh database generations in the
 * protected allocation watermarks (the first is retired, so a pending
 * management receipt planned for it resolves as aborted) and stages the
 * account service's own ownership-neutral local intent (accounts.local,
 * CIACTLC4). The installed ACCOUNT service finishes it at its next start with
 * its normal recovery (OWNER bindings checked, bootstrap.state advanced).
 * Interruption before the intent leaves only retired generations; after it,
 * recovery completes the same change. uid 500 stays disabled for normal
 * logon and becomes usable in Safe Mode (M-Q1); no session is started, no
 * retired uid is reused and no private data is reassigned.
 */
#include "setup.h"
#include "../accounts/accounts.h"
#include "../accounts/wire.h"
#include "../../sdk/private/provision.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int st_password_valid(const char *s) { return acct_password_valid(s, 0); }
int st_account_name_valid(const char *s)
{ return s && acct_name_valid(s) && !acct_reserved_name(s); }

void st_secret_wipe(void *p, size_t n) { acct_wipe(p, n); }

int st_seed_build(uint8_t out[CIUKI_PROVISION_SEED_BYTES], const char *name, const char *user_password,
                  const char *recovery_password, int (*entropy)(void *, void *, size_t), void *ctx)
{
    if (!out || !entropy || !st_account_name_valid(name) || !st_password_valid(user_password) ||
        !st_password_valid(recovery_password)) return -EINVAL;
    struct acct_db *d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    uint8_t salts[32];
    int e = entropy(ctx, salts, sizeof(salts));
    if (!e && !memcmp(salts, salts + 16, 16)) e = -EAGAIN;  /* never a shared salt */
    if (!e) {
        d->generation = 0; d->next_uid = 1001; d->count = 3; d->flags = 0;
        d->origin = ACCT_SETUP; d->auto_logon_uid = 0;
        struct acct_record *r = d->records;
        r[0] = (struct acct_record){ .uid = 500, .flags = ACCT_BUILTIN | ACCT_PASSWORD_SET, .groups = ACCT_ADMIN,
                                     .kdf = 1, .iterations = ACCT_ITERATIONS, .name = "CiukiAdministrator" };
        r[1] = (struct acct_record){ .uid = 501, .flags = ACCT_BUILTIN, .groups = ACCT_GUEST, .name = "Guest" };
        r[2] = (struct acct_record){ .uid = 1000, .flags = ACCT_ENABLED | ACCT_PASSWORD_SET, .groups = ACCT_ADMIN,
                                     .kdf = 1, .iterations = ACCT_ITERATIONS };
        memcpy(r[2].name, name, strlen(name));
        memcpy(r[0].salt, salts, 16); memcpy(r[2].salt, salts + 16, 16);
        e = acct_derive(recovery_password, r[0].salt, ACCT_ITERATIONS, r[0].verifier);
        if (!e) e = acct_derive(user_password, r[2].salt, ACCT_ITERATIONS, r[2].verifier);
    }
    size_t n = 0;
    if (!e) e = acct_encode(d, 1, out, CIUKI_PROVISION_SEED_BYTES, &n);
    if (!e && (n != CIUKI_PROVISION_SEED_BYTES || ciuki_provision_seed_header(out, n, ACCT_SETUP))) e = -EIO;
    if (e) acct_wipe(out, CIUKI_PROVISION_SEED_BYTES);
    acct_wipe(salts, sizeof(salts)); acct_wipe(d, sizeof(*d)); free(d);
    return e;
}

/* ---- confirmed lost-password repair ---- */
static const char *const unfinished[] = {
    "bootstrap.state.new", "bootstrap.state.bak", "bootstrap.state.txn", "bootstrap.state.tmp",
    "accounts.db.txn", "accounts.db.new", "accounts.db.bak", "accounts.local", "accounts.local.new",
    "accounts.local.bak", "owner.checkpoint.pending", "acct.commit", "acct.prepared",
};
static int present(const struct acct_io *io, const char *name)
{
    uint8_t probe[1]; size_t n = 0;
    return io->read(io->ctx, name, probe, sizeof(probe), &n) == -ENOENT ? 0 : 1;
}
/* Read-only parse of the two allocation slots (store.c watermarks()). */
static int allocation(const struct acct_io *io, uint64_t volume, const uint32_t sid[3],
                      uint64_t *high, uint32_t *uid, int *best)
{
    *high = 0; *uid = 1001; *best = -1;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t p[97], sum[32]; size_t n = 0;
        int e = io->read(io->ctx, i ? "accounts.alloc.1" : "accounts.alloc.0", p, sizeof(p), &n);
        if (e == -ENOENT) continue;
        if (e || n != 96 || memcmp(p, "CIUKIALC", 8) || a64(p + 8) != volume || !azero(p + 40, 24)) return -EIO;
        for (unsigned j = 0; j < 3; ++j) if (a32(p + 16 + 4 * j) != sid[j]) return -EIO;
        sha256(p, 64, sum);
        if (!acct_equal_secret(sum, p + 64, 32) || !a64(p + 32) || a32(p + 28) < 1001) return -EIO;
        if (a64(p + 32) > *high) {
            if (a32(p + 28) < *uid) return -EIO;
            *high = a64(p + 32); *uid = a32(p + 28); *best = (int)i;
        } else if (a32(p + 28) > *uid || (a64(p + 32) == *high && a32(p + 28) != *uid)) return -EIO;
    }
    return *best < 0 ? -EIO : 0;
}
static int reserve(const struct acct_io *io, uint64_t volume, const uint32_t sid[3], uint64_t g, uint32_t uid, int *best)
{
    uint8_t p[96] = {0};
    memcpy(p, "CIUKIALC", 8); w64(p + 8, volume);
    for (unsigned j = 0; j < 3; ++j) w32(p + 16 + 4 * j, sid[j]);
    w32(p + 28, uid); w64(p + 32, g); sha256(p, 64, p + 64);
    const char *name = *best == 0 ? "accounts.alloc.1" : "accounts.alloc.0";
    int e = io->write(io->ctx, name, p, sizeof(p));
    if (!e) e = io->flush(io->ctx, name);
    if (!e) e = io->barrier(io->ctx);
    if (!e) *best = *best == 0 ? 1 : 0;
    return e;
}
struct repair_frame {
    uint8_t bytes[ACCT_FILE_MAX + 1], next_bytes[ACCT_FILE_MAX], intent[64 + ACCT_FILE_MAX], state[CIUKI_PROVISION_STATE_BYTES + 1];
    struct acct_db db, next;
    struct acct_bootstrap boot;
};
static int inspect(const struct acct_io *io, struct st_repair *out, struct repair_frame *f, size_t *db_bytes)
{
    memset(out, 0, sizeof(*out));
    size_t n = 0;
    int e = io->read(io->ctx, "bootstrap.state", f->state, sizeof(f->state), &n);
    if (e == -ENOENT) return -ENOENT;                 /* not a provisioned CiukiOS installation */
    if (e || acct_boot_decode(&f->boot, f->state, n)) return -EIO;
    for (unsigned i = 0; i < sizeof(unfinished) / sizeof(*unfinished); ++i)
        if (present(io, unfinished[i])) { out->pending = 1; return -EBUSY; }
    if (f->boot.state != ACCT_CONSUMED) { out->pending = 1; return -EBUSY; }
    e = io->read(io->ctx, "accounts.db", f->bytes, sizeof(f->bytes), &n);
    if (e || acct_decode(&f->db, 0, f->bytes, n)) return -EIO;
    if (f->db.generation != f->boot.accounts_generation || memcmp(f->db.sid, f->boot.sid, 12) ||
        f->db.origin != f->boot.origin || !!(f->db.flags & ACCT_AUTO_RETIRED) != !!(f->boot.flags & ACCT_AUTO_RETIRED) ||
        f->db.records[0].uid != 500) return -EIO;
    e = allocation(io, f->boot.volume, f->db.sid, &out->high_generation, &out->high_uid, &out->slot);
    if (e || out->high_generation < f->db.generation || out->high_uid < f->db.next_uid) return -EIO;
    if (out->high_generation > UINT64_MAX - 2) return -EOVERFLOW;
    out->generation = f->db.generation; out->count = f->db.count;
    out->recovery_configured = !!(f->db.records[0].flags & ACCT_PASSWORD_SET);
    for (unsigned i = 0; i < f->db.count && i < ACCT_MAX; ++i) {
        out->uids[i] = f->db.records[i].uid;
        out->administrator[i] = f->db.records[i].groups == ACCT_ADMIN;
        out->enabled[i] = !!(f->db.records[i].flags & ACCT_ENABLED);
        memcpy(out->names[i], f->db.records[i].name, 32);
    }
    *db_bytes = n;
    return 0;
}
int st_repair_inspect(const struct acct_io *io, struct st_repair *out)
{
    if (!io || !out || !io->read) return -EINVAL;
    struct repair_frame *f = calloc(1, sizeof(*f));
    if (!f) return -ENOMEM;
    size_t n = 0;
    int e = inspect(io, out, f, &n);
    acct_wipe(f, sizeof(*f)); free(f);
    return e;
}
static void *repair_allocate(void *ctx, size_t n) { (void)ctx; return calloc(1, n); }
static void repair_deallocate(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
int st_repair_execute(const struct acct_io *io, const char *password, int (*entropy)(void *, void *, size_t), void *ctx,
                      struct st_repair *out)
{
    if (!io || !out || !entropy || !io->read || !io->write || !io->flush || !io->barrier || !io->rename || !io->remove)
        return -EINVAL;
    if (!st_password_valid(password)) return -EINVAL;
    struct repair_frame *f = calloc(1, sizeof(*f));
    if (!f) return -ENOMEM;
    size_t n = 0, m = 0;
    int e = inspect(io, out, f, &n);
    if (!e) {
        f->next = f->db; f->next.generation = out->high_generation + 2;
        struct acct_record *r = &f->next.records[0];
        e = entropy(ctx, r->salt, sizeof(r->salt));
        if (!e && azero(r->salt, sizeof(r->salt))) e = -EAGAIN;
        if (!e) e = acct_derive(password, r->salt, ACCT_ITERATIONS, r->verifier);
        if (!e) { r->kdf = 1; r->iterations = ACCT_ITERATIONS; r->flags = (r->flags | ACCT_PASSWORD_SET) & ~ACCT_CHANGE_REQUIRED; }
        if (!e) e = acct_encode(&f->next, 0, f->next_bytes, sizeof(f->next_bytes), &m);
    }
    /* Two durable reservations: G+1 retired, G+2 used (never reused later). */
    if (!e) e = reserve(io, f->boot.volume, f->db.sid, out->high_generation + 1, out->high_uid, &out->slot);
    if (!e) e = reserve(io, f->boot.volume, f->db.sid, out->high_generation + 2, out->high_uid, &out->slot);
    if (!e) {
        memcpy(f->intent, "CIACTLC4", 8); w32(f->intent + 8, (uint32_t)(64 + m)); w32(f->intent + 12, 0);
        w64(f->intent + 16, f->db.generation); memcpy(f->intent + 32, f->bytes + 64, 32);
        memcpy(f->intent + 64, f->next_bytes, m);
        struct acct_store st = {0};
        st.io = *io; st.volume = f->boot.volume;
        st.kernel.allocate = repair_allocate; st.kernel.deallocate = repair_deallocate;
        uint64_t sequence = 0;
        e = acct_blob_write(&st, "accounts.local", f->intent, 64 + m, &sequence);
    }
    if (!e) { out->next_generation = f->next.generation; out->pending = 1; }
    acct_wipe(f, sizeof(*f)); free(f);
    return e;
}
