/* SPDX-License-Identifier: MIT
 * Interim OWNER backend for images without sec-k02. It is the sealed
 * production table of this image, not a host fake: it holds no data and
 * refuses publication, so no owner transaction, receipt, checkpoint or
 * destination can exist until the real kernel OWNER endpoint is bound.
 */
#include "production.h"
#include <errno.h>
#include <stddef.h>

static int authenticate(void *ctx) { (void)ctx; return 0; }
static int generation(void *ctx, uint64_t *g) { (void)ctx; (void)g; return -ENOSYS; }
static int binding(void *ctx, uint64_t volume, uint32_t uid, char root[64], struct acct_owner_binding *b)
{ (void)ctx; (void)volume; (void)uid; (void)root; (void)b; return -ENOSYS; }
static int root_occupied(void *ctx, uint64_t volume, const char *root)
{ (void)ctx; (void)volume; (void)root; return -ENOSYS; }
static int stage(void *ctx, const struct acct_owner_request *q, void *manifest, size_t capacity, size_t *bytes)
{ (void)ctx; (void)q; (void)manifest; (void)capacity; (void)bytes; return -ENOSYS; }
static int tree_digest(void *ctx, uint64_t transaction, uint32_t uid, uint8_t digest[32])
{ (void)ctx; (void)transaction; (void)uid; (void)digest; return -ENOSYS; }
static int validate_stage(void *ctx, const struct acct_owner_request *q) { (void)ctx; (void)q; return -ENOSYS; }
static int snapshot(void *ctx, const struct acct_db *d, uint64_t high, uint32_t uid_high, void *out, size_t cap,
                    size_t *bytes, uint64_t *floor)
{ (void)ctx; (void)d; (void)high; (void)uid_high; (void)out; (void)cap; (void)bytes; (void)floor; return -ENOSYS; }
static int recover_orphans(void *ctx, uint64_t volume, uint64_t active)
{ (void)ctx; (void)volume; (void)active; return -ENOSYS; }
static int checkpoint_floor(void *ctx, uint64_t *floor, unsigned *receipts)
{ (void)ctx; (void)floor; (void)receipts; return -ENOSYS; }
static int call(void *ctx, const void *request, size_t n, void *reply, size_t cap, size_t *bytes)
{ (void)ctx; (void)request; (void)n; (void)reply; (void)cap; (void)bytes; return -ENOSYS; }

void acct_owner_interim(struct acct_owner *out)
{
    *out=(struct acct_owner){ .ctx=NULL, .real=1, .authenticate=authenticate, .generation=generation,
        .binding=binding, .root_occupied=root_occupied, .stage=stage, .tree_digest=tree_digest,
        .validate_stage=validate_stage, .snapshot=snapshot, .recover_orphans=recover_orphans,
        .checkpoint_floor=checkpoint_floor, .call=call };
}
