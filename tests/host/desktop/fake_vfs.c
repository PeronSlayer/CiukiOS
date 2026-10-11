/* SPDX-License-Identifier: MIT */
#include "fake_vfs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fake_node fake_nodes[FAKE_NODES];
static struct { int node, live; unsigned offset; } fds[16];
int fake_countdown, fake_operations, fake_metadata_count, fake_short_io;

static int injection(void) { fake_operations++; return fake_countdown > 0 && --fake_countdown == 0 ? -EIO : 0; }
static unsigned fold(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b)
{ while (*a && fold((unsigned char)*a) == fold((unsigned char)*b)) { a++; b++; } return !*a && !*b; }

int fake_lookup(const char *p) { for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live && same(fake_nodes[i].path, p)) return i; return -1; }

int fake_create(const char *p, enum fm_type type)
{
    if (fake_lookup(p) >= 0) return -EEXIST;
    for (int i = 0; i < FAKE_NODES; i++) if (!fake_nodes[i].live) {
        memset(&fake_nodes[i], 0, sizeof(fake_nodes[i]));
        fake_nodes[i].live = 1; snprintf(fake_nodes[i].path, FM_PATH, "%s", p);
        fake_nodes[i].meta = (struct fm_meta){type, 0, (uint64_t)i + 1, strncmp(p, "/mnt/d", 6) ? 1u : 2u,
                                               type == FM_DIR ? 0040777u : 0100666u, 1791700000 + i * 60, 0};
        return i;
    }
    return -ENOSPC;
}

int fake_file(const char *p, unsigned size, unsigned char seed)
{
    int n = fake_create(p, FM_FILE);
    assert(n >= 0 && size <= FAKE_DATA);
    fake_nodes[n].meta.size = size;
    for (unsigned i = 0; i < size; i++) fake_nodes[n].data[i] = (unsigned char)(seed + i % 251);
    return n;
}

static int parent_of(const char *p)
{
    char parent[FM_PATH]; fm_parent(parent, p);
    return fake_lookup(parent);
}

static int stat_node(void *ctx, const char *p, struct fm_meta *m)
{ (void)ctx; int r = injection(); if (r) return r; int n = fake_lookup(p); if (n < 0) return -ENOENT; *m = fake_nodes[n].meta; return 0; }

static int list(void *ctx, const char *p, fm_emit emit, void *arg)
{
    (void)ctx; int r = injection(); if (r) return r;
    int n = fake_lookup(p); if (n < 0) return -ENOENT;
    if (fake_nodes[n].meta.type != FM_DIR) return -ENOTDIR;
    /* Emit in creation order, like an unsorted FAT directory. */
    for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live && i != n && fm_beneath(fake_nodes[i].path, fake_nodes[n].path)) {
        const char *name = fake_nodes[i].path + strlen(fake_nodes[n].path);
        if (*name == '/') name++;
        if (*name && !strchr(name, '/')) { r = emit(name, &fake_nodes[i].meta, arg); if (r) return r; }
    }
    return 0;
}

static int mkdir_node(void *ctx, const char *p)
{ (void)ctx; int r = injection(); if (r) return r; if (parent_of(p) < 0) return -ENOENT; int n = fake_create(p, FM_DIR); return n < 0 ? n : 0; }

static int rename_node(void *ctx, const char *a, const char *b)
{
    (void)ctx; int r = injection(); if (r) return r;
    int source = fake_lookup(a), target = fake_lookup(b);
    if (source < 0) return -ENOENT;
    if (parent_of(b) < 0) return -ENOENT;
    unsigned volume = strncmp(b, "/mnt/d", 6) ? 1 : 2;
    if (fake_nodes[source].meta.dev != volume) return -EXDEV;
    if (fm_beneath(b, a) && !same(a, b)) return -EINVAL;
    if (target >= 0 && target != source) {
        if (fake_nodes[target].meta.type != fake_nodes[source].meta.type) return fake_nodes[source].meta.type == FM_DIR ? -ENOTDIR : -EISDIR;
        fake_nodes[target].live = 0;
    }
    char old[FM_PATH]; snprintf(old, sizeof(old), "%s", fake_nodes[source].path);
    for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live && fm_beneath(fake_nodes[i].path, old)) {
        char path[FM_PATH]; snprintf(path, sizeof(path), "%s%s", b, fake_nodes[i].path + strlen(old));
        snprintf(fake_nodes[i].path, FM_PATH, "%s", path);
    }
    return 0;
}

static int unlink_node(void *ctx, const char *p)
{ (void)ctx; int r = injection(); if (r) return r; int n = fake_lookup(p); if (n < 0) return -ENOENT; if (fake_nodes[n].meta.type != FM_FILE) return -EISDIR; fake_nodes[n].live = 0; return 0; }

static int rmdir_node(void *ctx, const char *p)
{
    (void)ctx; int r = injection(); if (r) return r;
    int n = fake_lookup(p); if (n < 0) return -ENOENT;
    for (int i = 0; i < FAKE_NODES; i++) if (i != n && fake_nodes[i].live && fm_beneath(fake_nodes[i].path, fake_nodes[n].path)) return -ENOTEMPTY;
    fake_nodes[n].live = 0; return 0;
}

static int open_node(void *ctx, const char *p, int make)
{
    (void)ctx; int r = injection(); if (r) return r;
    if (make && parent_of(p) < 0) return -ENOENT;
    int n = make ? fake_create(p, FM_FILE) : fake_lookup(p);
    if (n < 0) return make ? n : -ENOENT;
    for (int fd = 0; fd < 16; fd++) if (!fds[fd].live) { fds[fd].node = n; fds[fd].live = 1; fds[fd].offset = 0; return fd; }
    return -EMFILE;
}

static int read_node(void *ctx, int fd, void *p, unsigned size)
{
    (void)ctx; int r = injection(); if (r) return r;
    assert(fds[fd].live); struct fake_node *n = &fake_nodes[fds[fd].node];
    unsigned count = (unsigned)n->meta.size - fds[fd].offset;
    if (count > size) count = size;
    if (fake_short_io && count > 113) count = 113;
    memcpy(p, n->data + fds[fd].offset, count); fds[fd].offset += count;
    return (int)count;
}

static int write_node(void *ctx, int fd, const void *p, unsigned size)
{
    (void)ctx; int r = injection(); if (r) return r;
    assert(fds[fd].live); struct fake_node *n = &fake_nodes[fds[fd].node];
    if (fake_short_io && size > 71) size = 71;
    if (fds[fd].offset + size > sizeof(n->data)) return -ENOSPC;
    memcpy(n->data + fds[fd].offset, p, size); fds[fd].offset += size; n->meta.size = fds[fd].offset;
    return (int)size;
}

static int sync_node(void *ctx, int fd) { (void)ctx; assert(fds[fd].live); return injection(); }
static int close_node(void *ctx, int fd) { (void)ctx; assert(fds[fd].live); fds[fd].live = 0; return injection(); }
static int metadata_node(void *ctx, const char *p, const struct fm_meta *m)
{
    (void)ctx; int n = fake_lookup(p); assert(n >= 0);
    fake_nodes[n].meta.mode = m->mode; fake_nodes[n].meta.modified = m->modified; fake_metadata_count++;
    return 0;
}

struct fm_vfs fake_vfs = {NULL, list, stat_node, mkdir_node, rename_node, unlink_node, rmdir_node,
                          open_node, read_node, write_node, sync_node, close_node, metadata_node};

void fake_reset(void)
{
    memset(fake_nodes, 0, sizeof(fake_nodes)); memset(fds, 0, sizeof(fds));
    fake_countdown = fake_operations = fake_metadata_count = fake_short_io = 0;
    fake_create("/", FM_DIR);
}

void fake_assert_clean(void)
{
    for (int fd = 0; fd < 16; fd++) assert(!fds[fd].live);
    for (int i = 0; i < FAKE_NODES; i++) if (fake_nodes[i].live) assert(!strstr(fake_nodes[i].path, ".ciuki-copy"));
}

/* Names and kinds of the canonical image (scripts/build_image.py); the
 * write gate adds SYSTEM/LOGS. Timestamps are fixed for digests. */
void fake_image_tree(void)
{
    fake_reset();
    static const char *const dirs[] = {"/bin", "/system", "/system/assets", "/system/dos", "/system/licenses",
                                       "/system/tests", "/system/tests/lua-5.4.8-tests", "/system/LOGS"};
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) fake_create(dirs[i], FM_DIR);
    static const char *const programs[] = {"/bin/demo", "/bin/desktop", "/bin/files", "/bin/hello", "/bin/libc_smoke", "/bin/lua"};
    for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        int n = fake_file(programs[i], 1000 + 37 * i, 1); fake_nodes[n].meta.exec = 1; fake_nodes[n].meta.mode = 0100777;
    }
    fake_file("/system/BOOT.CFG", 16, 'a');
    fake_file("/system/VMM.ELF", 30000, 7);
    fake_file("/system/assets/ciuki-portrait.xrgb", 26214, 9);
    fake_file("/system/dos/DOSBOOT.IMG", 32768, 3);
    fake_file("/system/licenses/COPYING.NEWLIB", 4096, 4);
    fake_file("/system/licenses/SDK-MIT.txt", 1077, 5);
    fake_file("/system/licenses/lua-5.4.8.txt", 1090, 6);
    fake_file("/system/tests/app-gate.meta", 900, 8);
    fake_file("/system/tests/ciuki-f2.lua", 2100, 10);
}
