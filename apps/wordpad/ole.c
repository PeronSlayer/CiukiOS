/* SPDX-License-Identifier: MIT
 * OLE2 compound file reader: see ole.h. */
#include "ole.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define FREESECT 0xffffffffu
#define ENDOFCHAIN 0xfffffffeu
#define FATSECT 0xfffffffdu
#define DIFSECT 0xfffffffcu
#define MAXREGSECT 0xfffffffau
#define NOSTREAM 0xffffffffu

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static unsigned rd16(const uint8_t *p) { return (unsigned)p[1] << 8 | p[0]; }

int wp_is_ole(const void *buf, size_t n)
{
    static const uint8_t sig[8] = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
    return n >= 8 && !memcmp(buf, sig, 8);
}

/* The sector `sid` as a pointer into the file, or NULL when it lies outside. */
static const uint8_t *sector(const struct wp_ole *o, uint32_t sid)
{
    if (sid > MAXREGSECT) return NULL;
    uint64_t off = ((uint64_t)sid + 1) << o->sector_shift;
    if (off + ((uint64_t)1 << o->sector_shift) > o->len) return NULL;
    return o->buf + off;
}
/* Follows a FAT chain from `first`, copying `size` bytes (or the whole chain when size is SIZE_MAX) into out. */
static int read_chain(const struct wp_ole *o, uint32_t first, size_t size, uint8_t *out, size_t *got)
{
    size_t sec = (size_t)1 << o->sector_shift, n = 0;
    uint32_t sid = first, steps = 0;
    while (sid <= MAXREGSECT) {
        if (steps++ > o->nfat) return -EINVAL;   /* a loop */
        const uint8_t *s = sector(o, sid);
        if (!s) return -EINVAL;
        size_t take = size == SIZE_MAX ? sec : size - n < sec ? size - n : sec;
        if (out && take) memcpy(out + n, s, take);
        n += take;
        if (size != SIZE_MAX && n >= size) break;
        if (sid >= o->nfat) return -EINVAL;
        sid = o->fat[sid];
    }
    if (size != SIZE_MAX && n < size) return -EINVAL;
    *got = n;
    return 0;
}

int wp_ole_open(struct wp_ole *o, const uint8_t *buf, size_t len)
{
    memset(o, 0, sizeof(*o));
    if (len < 512 || len > WP_OLE_MAX || !wp_is_ole(buf, len)) return -EINVAL;
    o->buf = buf; o->len = len;
    unsigned major = rd16(buf + 0x1a), order = rd16(buf + 0x1c);
    o->sector_shift = rd16(buf + 0x1e); o->mini_shift = rd16(buf + 0x20);
    if (order != 0xfffe || (major != 3 && major != 4) || o->sector_shift != (major == 3 ? 9u : 12u) || o->mini_shift != 6) return -EINVAL;
    size_t sec = (size_t)1 << o->sector_shift;
    uint32_t nsec = (uint32_t)(len / sec);   /* sectors, counting the header as one */
    uint32_t nfatsec = rd32(buf + 0x2c), dirstart = rd32(buf + 0x30), minifatstart = rd32(buf + 0x3c), nminifatsec = rd32(buf + 0x40),
             difstart = rd32(buf + 0x44), ndifsec = rd32(buf + 0x48);
    o->cutoff = rd32(buf + 0x38);
    if (nfatsec == 0 || nfatsec > nsec || ndifsec > nsec || nminifatsec > nsec) return -EINVAL;
    /* the FAT sector numbers: 109 in the header, the rest in the DIFAT chain */
    uint32_t *ids = malloc((size_t)nfatsec * sizeof(*ids));
    if (!ids) return -ENOMEM;
    uint32_t have = 0;
    for (unsigned i = 0; i < 109 && have < nfatsec; i++) { uint32_t v = rd32(buf + 0x4c + 4 * i); if (v == FREESECT) break; ids[have++] = v; }
    uint32_t dif = difstart;
    for (uint32_t d = 0; d < ndifsec && have < nfatsec; d++) {
        const uint8_t *s = sector(o, dif);
        if (!s) { free(ids); return -EINVAL; }
        unsigned per = (unsigned)(sec / 4 - 1);
        for (unsigned i = 0; i < per && have < nfatsec; i++) { uint32_t v = rd32(s + 4 * i); if (v == FREESECT) break; ids[have++] = v; }
        dif = rd32(s + sec - 4);
    }
    if (have != nfatsec) { free(ids); return -EINVAL; }
    o->nfat = (uint32_t)(nfatsec * (sec / 4));
    o->fat = malloc((size_t)o->nfat * sizeof(*o->fat));
    if (!o->fat) { free(ids); return -ENOMEM; }
    for (uint32_t i = 0; i < nfatsec; i++) {
        const uint8_t *s = sector(o, ids[i]);
        if (!s) { free(ids); wp_ole_close(o); return -EINVAL; }
        for (unsigned k = 0; k < sec / 4; k++) o->fat[i * (sec / 4) + k] = rd32(s + 4 * k);
    }
    free(ids);
    /* the directory */
    size_t dirbytes = 0;
    int e = read_chain(o, dirstart, SIZE_MAX, NULL, &dirbytes);
    if (e || !dirbytes || dirbytes > (16u << 20)) { wp_ole_close(o); return -EINVAL; }
    o->dir = malloc(dirbytes);
    if (!o->dir) { wp_ole_close(o); return -ENOMEM; }
    e = read_chain(o, dirstart, SIZE_MAX, o->dir, &dirbytes);
    if (e) { wp_ole_close(o); return -EINVAL; }
    o->ndir = (uint32_t)(dirbytes / 128);
    if (!o->ndir || o->dir[0x42] != 5) { wp_ole_close(o); return -EINVAL; }   /* the root entry */
    /* the mini FAT and the mini stream */
    if (nminifatsec && minifatstart <= MAXREGSECT) {
        size_t mb = 0;
        e = read_chain(o, minifatstart, SIZE_MAX, NULL, &mb);
        if (e) { wp_ole_close(o); return -EINVAL; }
        uint8_t *tmp = malloc(mb);
        if (!tmp) { wp_ole_close(o); return -ENOMEM; }
        e = read_chain(o, minifatstart, SIZE_MAX, tmp, &mb);
        if (e) { free(tmp); wp_ole_close(o); return -EINVAL; }
        o->nminifat = (uint32_t)(mb / 4);
        o->minifat = malloc((size_t)o->nminifat * sizeof(*o->minifat) + 4);
        if (!o->minifat) { free(tmp); wp_ole_close(o); return -ENOMEM; }
        for (uint32_t i = 0; i < o->nminifat; i++) o->minifat[i] = rd32(tmp + 4 * i);
        free(tmp);
        uint32_t rootstart = rd32(o->dir + 0x74);
        uint64_t rootsize = rd32(o->dir + 0x78);   /* the high half is zero in a version 3 file and ignored: streams are small */
        if (rootsize && rootsize <= o->len && rootstart <= MAXREGSECT) {
            o->mini = malloc((size_t)rootsize);
            if (!o->mini) { wp_ole_close(o); return -ENOMEM; }
            size_t got;
            e = read_chain(o, rootstart, (size_t)rootsize, o->mini, &got);
            if (e) { wp_ole_close(o); return -EINVAL; }
            o->mini_len = (size_t)rootsize;
        }
    }
    return 0;
}
void wp_ole_close(struct wp_ole *o)
{
    free(o->fat); free(o->minifat); free(o->dir); free(o->mini);
    memset(o, 0, sizeof(*o));
}

/* Entry name as ASCII (non-ASCII characters become '?'); returns its length. */
static size_t entry_name(const uint8_t *e, char *out, size_t cap)
{
    unsigned bytes = rd16(e + 0x40);
    if (bytes < 2 || bytes > 64) { out[0] = 0; return 0; }
    size_t n = 0;
    for (unsigned i = 0; i + 1 < bytes && n + 1 < cap; i += 2) {
        unsigned ch = rd16(e + i);
        out[n++] = (char)(ch < 128 ? ch : '?');
    }
    out[n] = 0;
    return n;
}
static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
/* The entry of the root's child tree called `name` (iterative walk, bounded by the entry count). */
static const uint8_t *find_entry(const struct wp_ole *o, const char *name)
{
    uint32_t *stack = malloc((size_t)(o->ndir + 1) * sizeof(*stack));
    if (!stack) return NULL;
    unsigned sp = 0, visited = 0;
    uint32_t child = rd32(o->dir + 0x4c);
    const uint8_t *found = NULL;
    if (child != NOSTREAM && child < o->ndir) stack[sp++] = child;
    while (sp && visited++ <= o->ndir) {
        uint32_t id = stack[--sp];
        const uint8_t *e = o->dir + (size_t)id * 128;
        char nm[40];
        entry_name(e, nm, sizeof(nm));
        if (e[0x42] == 2 && ieq(nm, name)) { found = e; break; }
        uint32_t l = rd32(e + 0x44), r = rd32(e + 0x48);
        if (r != NOSTREAM && r < o->ndir && sp < o->ndir) stack[sp++] = r;
        if (l != NOSTREAM && l < o->ndir && sp < o->ndir) stack[sp++] = l;
    }
    free(stack);
    return found;
}
int wp_ole_read(const struct wp_ole *o, const char *name, uint8_t **data, size_t *len)
{
    const uint8_t *e = find_entry(o, name);
    if (!e) return -ENOENT;
    uint32_t start = rd32(e + 0x74);
    uint64_t size = rd32(e + 0x78);
    if (size > o->len) return -EINVAL;
    uint8_t *out = malloc(size ? (size_t)size : 1);
    if (!out) return -ENOMEM;
    if (size < o->cutoff) {   /* the mini stream: 64-byte sectors chained by the mini FAT */
        size_t n = 0, ms = (size_t)1 << o->mini_shift;
        uint32_t sid = start, steps = 0;
        while (n < size) {
            if (sid > MAXREGSECT || steps++ > o->nminifat || (uint64_t)sid * ms + ms > o->mini_len) { free(out); return -EINVAL; }
            size_t take = size - n < ms ? (size_t)size - n : ms;
            memcpy(out + n, o->mini + (size_t)sid * ms, take);
            n += take;
            if (n >= size) break;
            if (sid >= o->nminifat) { free(out); return -EINVAL; }
            sid = o->minifat[sid];
        }
    } else if (size) {
        size_t got;
        if (read_chain(o, start, (size_t)size, out, &got)) { free(out); return -EINVAL; }
    }
    *data = out; *len = (size_t)size;
    return 0;
}
