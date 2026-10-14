/* Boot-only read of the production registry grammar. No registry writes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/audio.h>
#include <ciuki/files.h>
#include <ciuki/cpu.h>
#include "../../../sdk/sysroot-overlay/include/ciuki/audio_preference.h"
#include "../../../sdk/libreg/internal.h"

/* Adapt only the pure libreg parser; storage/recovery remains in user space.
 * Local errno and allocations are used only while boot settings are loaded. */
static int parser_errno;
struct parser_alloc { size_t size; uint32_t phys, pages; };
static void *parser_malloc(size_t n)
{
    if (n > SIZE_MAX - sizeof(struct parser_alloc)) { parser_errno = ENOMEM; return 0; }
    struct parser_alloc *p;
    uint32_t phys = 0, pages = 0;
#ifndef FS_HOST
    /* The kernel heap stops at 2 KiB. Larger parser buffers use a bounded
     * contiguous page run, and every page is freed after the boot read. */
    if (n + sizeof(*p) > 2000) {
        if (n > 2u * REG_TEXT_MAX + 2u) { parser_errno = ENOMEM; return 0; }
        pages = PAGE_ALIGN_UP(n + sizeof(*p)) / PAGE_SIZE;
        phys = pmm_alloc_contig(pages);
        p = phys ? P2V(phys) : 0;
    } else
#endif
    p = kmalloc(sizeof(*p) + n);
    if (!p) { parser_errno = ENOMEM; return 0; }
    p->size = n; p->phys = phys; p->pages = pages; return p + 1;
}
static void parser_free(void *p)
{
    if (!p) return;
    struct parser_alloc *h = (struct parser_alloc *)p - 1;
    if (h->phys) {
        uint32_t phys = h->phys, pages = h->pages;
        for (unsigned i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
    } else kfree(h);
}
static void *parser_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) { parser_errno = ENOMEM; return 0; }
    void *p = parser_malloc(n * size); if (p) memset(p, 0, n * size); return p;
}
static void *parser_realloc(void *old, size_t n)
{
    void *p = parser_malloc(n); if (!p) return 0;
    if (old) { size_t size = ((struct parser_alloc *)old - 1)->size; memcpy(p, old, size < n ? size : n); parser_free(old); }
    return p;
}
static char *parser_strdup(const char *s)
{ size_t n = strlen(s) + 1; char *p = parser_malloc(n); if (p) memcpy(p, s, n); return p; }
static char *parser_strchr(const char *s, int c)
{ do { if (*s == c) return (char *)s; } while (*s++); return 0; }
static char *parser_strrchr(const char *s, int c)
{ char *last = 0; do { if (*s == c) last = (char *)s; } while (*s++); return last; }
static void *parser_memchr(const void *data, int c, size_t n)
{ const unsigned char *s = data; for (size_t i = 0; i < n; i++) if (s[i] == (unsigned char)c) return (void *)(s + i); return 0; }
static int parser_strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static unsigned long long parser_unsigned(const char *s, char **end, int base)
{
    (void)base; uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        unsigned digit = (unsigned)(*s++ - '0');
        if (v > (UINT64_MAX - digit) / 10) { parser_errno = EINVAL; v = UINT64_MAX; }
        else v = v * 10 + digit;
    }
    *end = (char *)s; return v;
}
static long long parser_signed(const char *s, char **end, int base)
{
    bool negative = *s == '-'; if (*s == '-' || *s == '+') s++;
    uint64_t v = parser_unsigned(s, end, base);
    if (v > (negative ? 2147483648ull : 2147483647ull)) { parser_errno = EINVAL; return 0; }
    return negative ? -(int64_t)v : (int64_t)v;
}
#define CIUKI_REG_PARSE_ONLY
#ifdef errno
#undef errno
#endif
#define errno parser_errno
#define malloc parser_malloc
#define calloc parser_calloc
#define realloc parser_realloc
#define free parser_free
#define strdup parser_strdup
#define strchr parser_strchr
#define strrchr parser_strrchr
#define memchr parser_memchr
#define strcmp parser_strcmp
#define strtoull parser_unsigned
#define strtoll parser_signed
#define snprintf ksnprintf
#include "../../../sdk/libreg/registry.c"

bool audio_registry_load(struct vfs *v, struct ciuki_mixer_master *m)
{
    static struct vfs_table table;
    if (!v || !v->volumes[2] || vfs_table_init(v, &table, 2)) return false;
    /* Do not read the canonical root in the middle of recovery. AUDIO.CFG
     * remains the durable fallback until libreg can recover the root. */
    int fd = vfs_open(&table, "C:\\System\\Registry\\System.reg.txn", VFS_READ, VFS_DENY_NONE, 0);
    if (fd >= 0) { vfs_close(&table, fd); vfs_table_destroy(&table); return false; }
    if (fd != -FS_ENOENT) { vfs_table_destroy(&table); return false; }
    fd = vfs_open(&table, "C:\\System\\Registry\\System.reg", VFS_READ, VFS_DENY_NONE, 0);
    struct fat_entry info;
    size_t capacity = 0;
    if (fd >= 0 && !vfs_stat(&table, "C:\\System\\Registry\\System.reg", &info) && info.size <= REG_TEXT_MAX)
        capacity = (size_t)info.size + 1;
    char *text = capacity ? parser_malloc(capacity) : 0;
    size_t n = 0; struct rt *tree = 0; bool ok = false;
    if (text && !vfs_read(&table, fd, text, capacity, &n) && n < capacity)
        tree = reg_parse("System", text, n);
    if (tree) {
        struct rk *key = reg_key(tree, CIUKI_MASTER_KEY);
        struct rv *value = key ? value_find(key, CIUKI_MASTER_VALUE) : 0;
        unsigned l, r, mute;
        if (value && value->type == REG_INT32 && ciuki_master_unpack(*(int32_t *)value->data, &l, &r, &mute)) {
            m->left = l; m->right = r; m->mute = mute;
            m->save_state = CIUKI_MIXER_SAVED; m->save_error = 0; ok = true;
        }
    }
    reg_tree_free(tree); parser_free(text);
    if (fd >= 0) vfs_close(&table, fd);
    vfs_table_destroy(&table); return ok;
}
