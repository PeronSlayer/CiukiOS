/* SPDX-License-Identifier: MIT
 * Count production font/FreeType allocations, inject OOM, check warm painting. */
#include <ciuki/font.h>
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
static size_t live, peak, calls, fail_at;
union allocation { max_align_t alignment; size_t size; };
void *__wrap_malloc(size_t n)
{
    if (++calls == fail_at || n > SIZE_MAX - sizeof(union allocation)) return NULL;
    union allocation *p = __real_malloc(sizeof(*p) + n);
    if (!p) return NULL;
    p->size = n; live += n; if (live > peak) peak = live;
    return p + 1;
}
void __wrap_free(void *ptr)
{
    if (!ptr) return;
    union allocation *p = (union allocation *)ptr - 1;
    live -= p->size; __real_free(p);
}
void *__wrap_calloc(size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n) return NULL;
    size_t bytes = n * size; unsigned char *p = __wrap_malloc(bytes);
    if (p) for (size_t i = 0; i < bytes; i++) p[i] = 0;
    return p;
}
void *__wrap_realloc(void *ptr, size_t n)
{
    if (!ptr) return __wrap_malloc(n);
    if (!n) { __wrap_free(ptr); return NULL; }
    size_t old = ((union allocation *)ptr - 1)->size;
    unsigned char *p = __wrap_malloc(n);
    if (!p) return NULL;
    for (size_t i = 0; i < old && i < n; i++) p[i] = ((unsigned char *)ptr)[i];
    __wrap_free(ptr); return p;
}
static int paint(void *ctx, int x, int y, const struct ciuki_font_bitmap *b)
{ (void)ctx; (void)x; (void)y; assert(b->coverage); return 0; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    size_t original = live; struct ciuki_font_face *face;
    assert(!ciuki_font_open(argv[1], &face));
    size_t face_bytes = live; unsigned g;
    assert(!ciuki_font_lookup(face, 'A', &g));
    struct ciuki_font_bitmap b;
    assert(!ciuki_font_raster(face, g, 853, &b)); ciuki_font_bitmap_free(&b);
    int32_t advance;
    assert(!ciuki_font_paint(face, 853, "Warm AVATAR", 11, 0, 20, paint, NULL, &advance));
    size_t before = calls;
    for (unsigned i = 0; i < 100; i++) assert(!ciuki_font_paint(face, 853, "Warm AVATAR", 11, 0, 20, paint, NULL, &advance));
    assert(calls == before);
    for (unsigned size = 512; size <= 8192; size += 512) for (unsigned cp = 33; cp < 160; cp++) {
        assert(!ciuki_font_lookup(face, cp, &g)); assert(!ciuki_font_raster(face, g, size, &b)); ciuki_font_bitmap_free(&b);
        struct ciuki_font_cache_stats s; assert(!ciuki_font_cache_info(face, &s));
        assert(s.glyphs <= CIUKI_FONT_CACHE_GLYPHS && s.bytes <= CIUKI_FONT_CACHE_BYTES);
    }
    size_t max_bytes = peak;
    ciuki_font_close(face); assert(live == original);
    /* Every allocation boundary during open, load, raster and snapshot. */
    for (size_t fault = 1; fault < 256; fault++) {
        before = calls; fail_at = calls + fault;
        if (!ciuki_font_open(argv[1], &face)) {
            assert(!ciuki_font_lookup(face, 'g', &g));
            int result = ciuki_font_raster(face, g, 853, &b);
            if (!result) ciuki_font_bitmap_free(&b);
            else assert(errno == ENOMEM);
            ciuki_font_close(face);
        } else assert(errno == ENOMEM);
        assert(live == original);
        if (calls < fail_at) break;
        assert(calls > before);
    }
    printf("{\"face_live_bytes\":%zu,\"peak_live_bytes\":%zu,\"warm_paint_allocations\":0,\"leaked_bytes\":%zu}\n", face_bytes - original, max_bytes - original, live - original);
    return 0;
}
