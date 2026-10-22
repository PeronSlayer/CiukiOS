/* CiukiOS replacement for pixman's internal header, region code only.
 *
 * This is NOT upstream pixman-private.h. It supplies exactly what the
 * unmodified upstream pixman-region.c / pixman-region32.c (pixman 0.46.4,
 * see README.md) need to build as a private part of the desktop compositor:
 * the 32-bit region types, copied verbatim from upstream pixman.h (MIT,
 * COPYING), the assertion macros of upstream pixman-private.h and the
 * _pixman_log_error message of upstream pixman-utils.c.
 *
 * Every region function gets internal linkage (PIXMAN_EXPORT below), so the
 * compiler keeps only what the compositor calls: the floating-point entry
 * points (_init_rectf, _union_rectf, _intersect_rectf, _translatef,
 * _contains_pointf) and the a1-bitmap converter (_init_from_image) are never
 * emitted, and the integer-only compositor audit of build_desktop.py holds.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_PIXMAN_PRIVATE_H
#define CIUKI_PIXMAN_PRIVATE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIXMAN_EXPORT static __attribute__((unused))
/* The includer may supply a fixed storage pool for region data. */
#ifdef CIUKI_PIXMAN_ALLOC
#define malloc(n) CIUKI_PIXMAN_ALLOC(n)
#define realloc(p, n) CIUKI_PIXMAN_REALLOC(p, n)
#define free(p) CIUKI_PIXMAN_FREE(p)
#endif
#define FUNC ((const char *)(__func__))
#define unlikely(expr) __builtin_expect((expr), 0)
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Upstream pixman.h, 32-bit regions. */
typedef int pixman_bool_t;
typedef enum
{
    PIXMAN_REGION_OUT,
    PIXMAN_REGION_IN,
    PIXMAN_REGION_PART
} pixman_region_overlap_t;
typedef struct pixman_region32_data	pixman_region32_data_t;
typedef struct pixman_box32		pixman_box32_t;
typedef struct pixman_region32		pixman_region32_t;
struct pixman_region32_data {
    long		size;
    long		numRects;
/*  pixman_box32_t	rects[size];   in memory but not explicitly declared */
};
struct pixman_box32
{
    int32_t x1, y1, x2, y2;
};
struct pixman_region32
{
    pixman_box32_t          extents;
    pixman_region32_data_t  *data;
};
/* Upstream defines _print without PIXMAN_EXPORT (external linkage). This
 * prior static declaration gives that definition internal linkage too
 * (C11 6.2.2p4), so the compositor (libdeskui) and NetSurf's surface
 * transport can both include the region code in one program. */
static __attribute__((unused)) int pixman_region32_print(pixman_region32_t *region);

/* Only for the never-emitted _init_from_image: an a1 image has no source in
 * CiukiOS, every accessor reports an empty image. */
typedef enum { BITS } image_type_t;
typedef enum { PIXMAN_a1 = 0x01011000 } pixman_format_code_t;
typedef union pixman_image {
    image_type_t type;
    struct { image_type_t type; pixman_format_code_t format; } bits;
} pixman_image_t;
static inline uint32_t *pixman_image_get_data(pixman_image_t *i) { (void)i; return NULL; }
static inline int pixman_image_get_width(pixman_image_t *i) { (void)i; return 0; }
static inline int pixman_image_get_height(pixman_image_t *i) { (void)i; return 0; }
static inline int pixman_image_get_stride(pixman_image_t *i) { (void)i; return 0; }
#define SCREEN_SHIFT_LEFT(x, n) ((x) << (n))   /* upstream, little-endian */
#define SCREEN_SHIFT_RIGHT(x, n) ((x) >> (n))
#ifndef MIN
#define MIN(a, b) ((a < b) ? a : b)
#endif
#ifndef MAX
#define MAX(a, b) ((a > b) ? a : b)
#endif
/* Upstream declares this in pixman.h; it is called before its definition. */
static pixman_bool_t __attribute__((unused))
pixman_region32_union (pixman_region32_t *new_reg, const pixman_region32_t *reg1, const pixman_region32_t *reg2);

/* Upstream pixman-utils.c message; upstream pixman-private.h macros. */
static void __attribute__((unused))
_pixman_log_error (const char *function, const char *message)
{
    static int n_messages = 0;
    if (n_messages < 10)
    {
	fprintf (stderr, "*** BUG ***\nIn %s: %s\n", function, message);
	n_messages++;
    }
}
#define return_if_fail(expr)                                            \
    do                                                                  \
    {                                                                   \
	if (unlikely (!(expr)))                                         \
	{								\
	    _pixman_log_error (FUNC, "The expression " # expr " was false"); \
	    return;							\
	}								\
    }                                                                   \
    while (0)
#define return_val_if_fail(expr, retval)                                \
    do                                                                  \
    {                                                                   \
	if (unlikely (!(expr)))                                         \
	{								\
	    _pixman_log_error (FUNC, "The expression " # expr " was false"); \
	    return (retval);						\
	}								\
    }                                                                   \
    while (0)
#define critical_if_fail(expr)						\
    do									\
    {									\
	if (unlikely (!(expr)))                                         \
	    _pixman_log_error (FUNC, "The expression " # expr " was false"); \
    }									\
    while (0)
#endif
