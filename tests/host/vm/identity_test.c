/* Actual VFS identity adapter with faulting namespace/transport seams.
 * SPDX-License-Identifier: GPL-2.0-only */
#define PORT_TEST_MAIN unused_port_suite_main
#include "port_test.c"
#include "../../../src/kernel/net/bridge_identity.c"

static struct vfs_table files;
static struct { uint8_t bytes[17]; size_t length; bool exists; } id_records[2];
static unsigned handles, commits, renames, flushes, creates;
static int fail_open, fail_read, fail_write, fail_commit, fail_close, fail_rename, fail_flush;
static bool short_write;
static char order[64]; static unsigned ordered;
static void event(char c) { CHECK(ordered + 1 < sizeof(order)); order[ordered++] = c; order[ordered] = 0; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share deny, uint8_t attr)
{
    CHECK(t == &files); CHECK(!strcmp(path, identity_path) || !strcmp(path, pending_path));
    bool pending = !strcmp(path, pending_path);
    if (fail_open) return fail_open;
    if (flags & VFS_WRITE) {
        CHECK(pending && flags == (VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE));
        CHECK(deny == VFS_DENY_ALL && attr == (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM));
        if (id_records[1].exists) return -FS_EEXIST;
        id_records[1].exists = true; id_records[1].length = 0; creates++; event('O');
    } else {
        CHECK(flags == VFS_READ && deny == VFS_DENY_WRITE && !attr);
        if (!id_records[pending].exists) return -FS_ENOENT;
        event('o');
    }
    CHECK(!handles); handles++; return pending;
}
int vfs_read(struct vfs_table *t, int h, void *out, size_t n, size_t *done)
{
    CHECK(t == &files && handles == 1 && h >= 0 && h < 2 && n == 17);
    event('r'); *done = 0;
    if (fail_read) return fail_read;
    *done = id_records[h].length; memcpy(out, id_records[h].bytes, *done); return 0;
}
int vfs_write(struct vfs_table *t, int h, const void *data, size_t n, size_t *done)
{
    CHECK(t == &files && h == 1 && handles == 1 && n == 16); event('w');
    *done = 0; if (fail_write) return fail_write;
    *done = short_write ? 7 : n;
    memcpy(id_records[1].bytes, data, *done); id_records[1].length = *done; return 0;
}
int vfs_commit(struct vfs_table *t, int h)
{ CHECK(t == &files && handles == 1 && h >= 0 && h < 2); commits++; event('c'); return fail_commit; }
int vfs_close(struct vfs_table *t, int h)
{ CHECK(t == &files && handles == 1 && h >= 0 && h < 2); handles--; event('x'); return fail_close; }
int vfs_rename(struct vfs_table *t, const char *from, const char *to)
{
    CHECK(t == &files && !handles && !strcmp(from, pending_path) && !strcmp(to, identity_path));
    renames++; event('n'); if (fail_rename) return fail_rename;
    CHECK(id_records[1].exists); if (id_records[0].exists) return -FS_EEXIST;
    id_records[0] = id_records[1]; id_records[1].exists = false; return 0;
}
int vfs_flush_drive(struct vfs_table *t, unsigned drive)
{ CHECK(t == &files && drive == 2 && !handles); flushes++; event('f'); return fail_flush; }
static const struct bridge_identity_ops file_ops = {
    &files, net_bridge_identity_read_file, net_bridge_identity_write_file, identity_random
};
static void reset(void)
{
    CHECK(!handles); memset(id_records, 0, sizeof(id_records));
    ordered = commits = renames = flushes = creates = randoms = 0; order[0] = 0;
    fail_open = fail_read = fail_write = fail_commit = fail_close = fail_rename = fail_flush = 0;
    short_write = zero_id = false; random_error = 0;
}
int main(void)
{
    uint8_t id[16], original[16], zero[16] = {0}; reset();
    CHECK(!net_bridge_identity_load(&file_ops, id)); memcpy(original, id, 16);
    CHECK(randoms == 1 && creates == 1 && commits == 1 && renames == 1 && flushes == 1);
    CHECK(!strcmp(order, "Owcxnf") && id_records[0].length == 16 && !id_records[1].exists);
    ordered = 0; order[0] = 0;
    CHECK(!net_bridge_identity_load(&file_ops, id) && !memcmp(id, original, 16));
    CHECK(randoms == 1 && creates == 1 && !strcmp(order, "orcx"));
    /* Existing malformed id_records are never overwritten, including a valid
     * pending candidate next to them. An existing valid final record wins. */
    for (unsigned length = 0; length <= 17; length++) if (length != 16) {
        reset(); id_records[0].exists = true; id_records[0].length = length;
        CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EINVAL && !memcmp(id, zero, 16));
        CHECK(!randoms && !creates && !renames && !handles);
    }
    reset(); id_records[0].exists = true; id_records[0].length = 16;
    CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EINVAL && !randoms);
    reset(); id_records[1].exists = true; id_records[1].length = 16; memcpy(id_records[1].bytes, original, 16);
    CHECK(!net_bridge_identity_load(&file_ops, id) && !memcmp(id, original, 16));
    CHECK(!randoms && !creates && commits == 1 && renames == 1 && flushes == 1);
    CHECK(!strcmp(order, "orcxnf"));
    reset(); id_records[1].exists = true; id_records[1].length = 7;
    CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EINVAL && !randoms && !renames);
    /* Every publication failure refuses admission and closes its handle. */
    int *faults[] = {&fail_write, &fail_commit, &fail_close, &fail_rename, &fail_flush};
    for (unsigned i = 0; i < ARRAY_SIZE(faults); i++) {
        reset(); *faults[i] = -FS_EIO;
        CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EIO && !memcmp(id, zero, 16));
        CHECK(!handles && randoms == 1 && creates == 1);
        *faults[i] = 0;
        if (i) {
            CHECK(!net_bridge_identity_load(&file_ops, id) && !memcmp(id, original, 16));
            CHECK(randoms == 1 && creates == 1);
        } else CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EINVAL && randoms == 1);
    }
    reset(); short_write = true;
    CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EIO && !renames && !flushes);
    reset(); fail_open = -FS_EROFS;
    CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EROFS && !randoms && !creates);
    reset(); id_records[0].exists = true; id_records[0].length = 16; memcpy(id_records[0].bytes, original, 16);
    fail_read = -FS_EIO;
    CHECK(net_bridge_identity_load(&file_ops, id) == -FS_EIO && !handles && !randoms);
    reset(); id_records[0].exists = true; id_records[0].length = 16; memcpy(id_records[0].bytes, original, 16);
    CHECK(net_bridge_identity_write_file(&files, original) == -FS_EEXIST);
    CHECK(!memcmp(id_records[0].bytes, original, 16) && !flushes);
    CHECK(net_bridge_identity_write_file(&files, zero) == -FS_EINVAL);
    CHECK(net_bridge_identity_read_file(0, id) == -FS_EINVAL);
    CHECK(net_bridge_identity_write_file(0, original) == -FS_EINVAL);
    printf("NETID VFS adapter: %u checks passed\n", checks); return 0;
}
