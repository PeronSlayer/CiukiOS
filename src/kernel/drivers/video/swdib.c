/* Software reference engine; original fbdev integer conversion paths.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
struct pixel_layout {
    unsigned bytes, red_shift, red_pos, green_shift, green_pos, blue_shift, blue_pos;
};

static struct pixel_layout pixel_layout(const struct fb_device *d)
{
    return (struct pixel_layout){ d->bpp / 8, 8 - d->red_size, d->red_pos,
                                 8 - d->green_size, d->green_pos, 8 - d->blue_size, d->blue_pos };
}

static uint32_t pack_layout(const struct pixel_layout *l, uint32_t rgb)
{
    return ((((rgb >> 16) & 255u) >> l->red_shift) << l->red_pos) |
           ((((rgb >> 8) & 255u) >> l->green_shift) << l->green_pos) |
           (((rgb & 255u) >> l->blue_shift) << l->blue_pos);
}

uint32_t swdib_pack(const struct fb_device *d, uint32_t rgb)
{
    const struct pixel_layout l = pixel_layout(d);
    return pack_layout(&l, rgb);
}

/* Unaligned RAM loads and generic integer stores are valid on i686. The
 * lowered C alignment and alias permission also cover arbitrary row pitch. */
typedef uint32_t fb_word __attribute__((aligned(1), may_alias));
typedef uint16_t fb_half __attribute__((aligned(1), may_alias));

static void pixel(uint8_t *p, unsigned bytes, uint32_t colour)
{
    if (bytes == 2)
        *(fb_half *)p = (uint16_t)colour;
    else if (bytes == 4)
        *(fb_word *)p = colour;
    else {
        p[0] = (uint8_t)colour;
        p[1] = (uint8_t)(colour >> 8);
        p[2] = (uint8_t)(colour >> 16);
    }
}

/* Intel SDM Vol. 3A, 253668-084US, sections 12.3/12.3.1 and 9.2.5:
 * https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf
 * Consecutive WC writes combine; partially filled buffers drain at the next
 * serializing event. A compiler barrier per row preserves generated stores,
 * without volatile per pixel or a hardware fence per row. Keep the existing
 * framebuffer_write_drain() before ownership release and the end timestamp. */
static void row_written(void) { __asm__ volatile("" ::: "memory"); }

/* Both pointers must be dword aligned; count is in dwords, not bytes. */
static void row_copy(uint8_t *dst, const void *src, size_t count)
{
    __asm__ volatile("cld\n\trep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory", "cc");
}

static void row_fill(uint8_t *dst, uint32_t colour, size_t count)
{
    __asm__ volatile("cld\n\trep stosl" : "+D"(dst), "+c"(count) : "a"(colour) : "memory", "cc");
}

static void present_xrgb_row(uint8_t *dst, const uint8_t *src, unsigned width)
{
    uint32_t used = 0;
    for (unsigned x = 0; x < width; x++) used |= ((const fb_word *)src)[x];
    if (!(used & 0xff000000u)) {
        row_copy(dst, src, width);
        return;
    }
    /* X is ignored by the public surface contract. Normalize in RAM, never
     * by reading or masking a destination word in the WC framebuffer. */
    uint32_t pixels[64];
    while (width) {
        unsigned n = width > ARRAY_SIZE(pixels) ? ARRAY_SIZE(pixels) : width;
        for (unsigned x = 0; x < n; x++) pixels[x] = ((const fb_word *)src)[x] & 0xffffffu;
        row_copy(dst, pixels, n);
        src += n * 4; dst += n * 4; width -= n;
    }
}

static uint16_t pack_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

static void present_565_row(uint8_t *dst, const uint8_t *src, unsigned width)
{
    if ((uintptr_t)dst & 2u) {
        pixel(dst, 2, pack_565(*(const fb_word *)src));
        dst += 2; src += 4; width--;
    }
    uint16_t pixels[64] __attribute__((aligned(4)));
    while (width >= 2) {
        unsigned n = width > ARRAY_SIZE(pixels) ? ARRAY_SIZE(pixels) : width & ~1u;
        for (unsigned x = 0; x < n; x++) pixels[x] = pack_565(((const fb_word *)src)[x]);
        row_copy(dst, pixels, n / 2);
        src += n * 4; dst += n * 2; width -= n;
    }
    if (width) pixel(dst, 2, pack_565(*(const fb_word *)src));
}

void swdib_present(const struct fb_device *d, const struct fb_surface *s, const struct fb_rect *r, const struct fb_rect *c)
{
    uint32_t sx = (uint32_t)((int64_t)c->x - r->x), sy = (uint32_t)((int64_t)c->y - r->y);
    const struct pixel_layout l = pixel_layout(d);
    bool xrgb = l.bytes == 4 && !l.red_shift && l.red_pos == 16 &&
                !l.green_shift && l.green_pos == 8 && !l.blue_shift && !l.blue_pos;
    bool rgb565 = l.bytes == 2 && l.red_shift == 3 && l.red_pos == 11 &&
                  l.green_shift == 2 && l.green_pos == 5 && l.blue_shift == 3 && !l.blue_pos;
    for (int32_t y = 0; y < c->height; y++) {
        const uint8_t *sp = (const uint8_t *)s->pixels + (sy + (uint32_t)y) * s->pitch + sx * 4;
        uint8_t *dp = d->mapped + (c->y + (uint32_t)y) * d->pitch + (uint32_t)c->x * l.bytes;
        if (xrgb && !(((uintptr_t)sp | (uintptr_t)dp) & 3u))
            present_xrgb_row(dp, sp, (unsigned)c->width);
        else if (rgb565 && !((uintptr_t)dp & 1u))
            present_565_row(dp, sp, (unsigned)c->width);
        else for (int32_t x = 0; x < c->width; x++)
            pixel(dp + (uint32_t)x * l.bytes, l.bytes, pack_layout(&l, ((const fb_word *)sp)[x]));
        row_written();
    }
}

void swdib_fill(const struct fb_device *d, const struct fb_rect *c, uint32_t colour)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t y = 0; y < c->height; y++) {
        uint8_t *dp = d->mapped + (c->y + (uint32_t)y) * d->pitch + (uint32_t)c->x * bytes;
        unsigned width = (unsigned)c->width;
        if (bytes == 4 && !((uintptr_t)dp & 3u))
            row_fill(dp, colour, width);
        else if (bytes == 2 && !((uintptr_t)dp & 1u)) {
            if ((uintptr_t)dp & 2u) { pixel(dp, 2, colour); dp += 2; width--; }
            row_fill(dp, colour | (colour << 16), width / 2);
            if (width & 1u) pixel(dp + (width & ~1u) * 2, 2, colour);
        } else for (int32_t x = 0; x < c->width; x++)
            pixel(dp + (uint32_t)x * bytes, bytes, colour);
        row_written();
    }
}

void swdib_copy(const struct fb_device *d, const struct fb_rect *r, int32_t x, int32_t y)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t i = 0; i < r->height; i++) {
        int32_t row = y > r->y ? r->height - 1 - i : i;
        uint8_t *dst = d->mapped + (uint32_t)(y + row)*d->pitch + (uint32_t)x*bytes;
        const uint8_t *src = d->mapped + (uint32_t)(r->y + row)*d->pitch + (uint32_t)r->x*bytes;
        size_t n = (size_t)r->width*bytes;
        if (y == r->y && x > r->x) while (n) { n--; dst[n] = src[n]; }
        else for (size_t j = 0; j < n; j++) dst[j] = src[j];
        row_written();
    }
}
void swdib_pattern(const struct fb_device *d, const struct fb_rect *c, const uint8_t pattern[8], uint32_t fg, uint32_t bg)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t y = c->y; y < c->y + c->height; y++) {
        for (int32_t x = c->x; x < c->x + c->width; x++)
            pixel(d->mapped + (uint32_t)y*d->pitch + (uint32_t)x*bytes, bytes,
                  pattern[y & 7] & (0x80u >> (x & 7)) ? fg : bg);
        row_written();
    }
}
void swdib_convert(const struct fb_device *d, const struct fb_rect *dst, void *rows, uint32_t stride)
{
    const struct pixel_layout l = pixel_layout(d);
    for (int32_t y = 0; y < dst->height; y++) {
        uint8_t *row = (uint8_t *)rows + (uint32_t)y * stride;
        for (int32_t x = 0; x < dst->width; x++) {
            uint32_t v = pack_layout(&l, ((const fb_word *)row)[x]);
            pixel(row + (uint32_t)x*l.bytes, l.bytes, v);
        }
    }
}
void swdib_write(const struct fb_device *d, const struct fb_rect *dst, const struct fb_rect *c, const void *rows, uint32_t stride)
{
    unsigned bytes = d->bpp / 8;
    for (int32_t y = c->y; y < c->y + c->height; y++) {
        const uint8_t *src = (const uint8_t *)rows + (uint32_t)(y - dst->y)*stride + (uint32_t)(c->x - dst->x)*bytes;
        uint8_t *dp = d->mapped + (uint32_t)y*d->pitch + (uint32_t)c->x*bytes;
        if (bytes == 4 && !(((uintptr_t)dp | (uintptr_t)src) & 3u)) row_copy(dp, src, (size_t)c->width);
        else for (uint32_t i = 0; i < (uint32_t)c->width*bytes; i++) dp[i] = src[i];
        row_written();
    }
}

/* Part C arithmetic is original code implementing the approved contract.
 * Linux v6.12 cfbimgblt.c documents MSB-left expansion; EXA exa.h documents
 * preparation refusal and source/destination synchronization. No donor code. */
static uint32_t rgb_mask(const struct fb_device *d)
{
    return (((1u<<d->red_size)-1)<<d->red_pos) |
           (((1u<<d->green_size)-1)<<d->green_pos) |
           (((1u<<d->blue_size)-1)<<d->blue_pos);
}
static uint32_t load_pixel(const struct fb_device *d, unsigned x, unsigned y)
{
    const uint8_t *p=d->mapped+(size_t)y*d->pitch+(size_t)x*(d->bpp/8);
    uint32_t v=0;
    for (unsigned b=0;b<d->bpp/8;b++) v|=(uint32_t)p[b]<<(b*8);
    return v;
}
static void store_pixel(const struct fb_device *d, unsigned x, unsigned y, uint32_t v)
{ pixel(d->mapped+(size_t)y*d->pitch+(size_t)x*(d->bpp/8),d->bpp/8,v); }
static unsigned expand(unsigned v, unsigned n)
{ unsigned m=(1u<<n)-1; return (255*v+m/2)/m; }
static uint32_t decode(const struct fb_device *d, uint32_t v)
{
    return expand((v>>d->red_pos)&((1u<<d->red_size)-1),d->red_size)<<16 |
           expand((v>>d->green_pos)&((1u<<d->green_size)-1),d->green_size)<<8 |
           expand((v>>d->blue_pos)&((1u<<d->blue_size)-1),d->blue_size);
}
uint32_t swdib_cursor_argb(const struct fb_device *d, uint32_t under, uint32_t argb)
{
    unsigned a=argb>>24;
    if (!a) return under;
    uint32_t rgb=decode(d,under), out=0;
    for (unsigned i=0;i<24;i+=8) {
        unsigned v=((argb>>i)&255)+((((rgb>>i)&255)*(255-a)+127)/255);
        out|=(v>255 ? 255 : v)<<i;
    }
    return swdib_pack(d,out);
}
uint32_t swdib_cursor_mono(const struct fb_device *d, uint32_t under, unsigned a, unsigned x, uint32_t fg, uint32_t bg)
{ return a ? (x ? under^rgb_mask(d) : under) : swdib_pack(d,x ? fg : bg); }
int swdib_target_valid(const struct swdib_target *t)
{
    if (!t || !t->identity) return -EINVAL;
    const struct fb_device *d=&t->device;
    if (!d->width || !d->height || d->width>CIUKI_BATCH_MAX_DIMENSION || d->height>CIUKI_BATCH_MAX_DIMENSION ||
        d->size>CIUKI_BATCH_MAX_EXTENT) return -E2BIG;
    if (d->bpp!=16 && d->bpp!=24 && d->bpp!=32) return -EOPNOTSUPP;
    if ((uint64_t)d->width*(d->bpp/8)>d->pitch || (uint64_t)(d->height-1)*d->pitch+(uint64_t)d->width*(d->bpp/8)>d->size) return -EINVAL;
    unsigned used=0;
    const unsigned n[]={d->red_size,d->green_size,d->blue_size}, p[]={d->red_pos,d->green_pos,d->blue_pos};
    for (unsigned i=0;i<3;i++) {
        if (!n[i] || n[i]>8 || p[i]+n[i]>d->bpp) return -EOPNOTSUPP;
        unsigned m=((1u<<n[i])-1)<<p[i];
        if (m&used) return -EINVAL;
        used|=m;
    }
    if (d->rsvd_size && (d->rsvd_size>8 || d->rsvd_pos+d->rsvd_size>d->bpp ||
        (used&(((1u<<d->rsvd_size)-1)<<d->rsvd_pos)))) return -EINVAL;
    if (t->offset>UINT64_MAX-d->size || (d->mapped && (uintptr_t)d->mapped>UINTPTR_MAX-d->size)) return -EOVERFLOW;
    return 0;
}
static bool coordinate(int32_t v)
{ return v>=-(int32_t)CIUKI_BATCH_MAX_COORDINATE && v<=(int32_t)CIUKI_BATCH_MAX_COORDINATE; }
static bool same_layout(const struct fb_device *a, const struct fb_device *b)
{
    return a->bpp==b->bpp && a->red_size==b->red_size && a->red_pos==b->red_pos &&
        a->green_size==b->green_size && a->green_pos==b->green_pos && a->blue_size==b->blue_size && a->blue_pos==b->blue_pos;
}
/* Compare clipped row byte intervals in allocation coordinates. A bounding
 * interval would falsely reject disjoint columns across multiple rows. */
static bool rectangles_overlap(const struct swdib_target *s, int32_t sx, int32_t sy, uint32_t sw, uint32_t sh,
                               const struct swdib_target *d, int32_t dx, int32_t dy, uint32_t dw, uint32_t dh)
{
    int64_t sl=sx<0 ? 0 : sx, st=sy<0 ? 0 : sy, sr=(int64_t)sx+sw, sb=(int64_t)sy+sh;
    int64_t dl=dx<0 ? 0 : dx, dt=dy<0 ? 0 : dy, dr=(int64_t)dx+dw, db=(int64_t)dy+dh;
    if (sr>s->device.width) sr=s->device.width;
    if (sb>s->device.height) sb=s->device.height;
    if (dr>d->device.width) dr=d->device.width;
    if (db>d->device.height) db=d->device.height;
    if (sl>=sr || st>=sb || dl>=dr || dt>=db) return false;
    while (st<sb && dt<db) {
        uint64_t sa=s->offset+st*s->device.pitch+sl*(s->device.bpp/8), se=sa+(sr-sl)*(s->device.bpp/8);
        uint64_t da=d->offset+dt*d->device.pitch+dl*(d->device.bpp/8), de=da+(dr-dl)*(d->device.bpp/8);
        if (sa<de && da<se) return true;
        if (se<=da) st++; else dt++;
    }
    return false;
}
int swdib_validate(const struct swdib_command *c)
{
    const struct ciuki_batch_record *r=c->record;
    if (!r || r->record_bytes!=128) return -EINVAL;
    if (r->opcode<CIUKI_CMD_FILL || r->opcode>CIUKI_CMD_CURSOR_SHOW) return -EOPNOTSUPP;
    if (r->dw>CIUKI_BATCH_MAX_DIMENSION || r->dh>CIUKI_BATCH_MAX_DIMENSION || r->sw>CIUKI_BATCH_MAX_DIMENSION || r->sh>CIUKI_BATCH_MAX_DIMENSION ||
        !coordinate(r->dx) || !coordinate(r->dy) || !coordinate(r->sx) || !coordinate(r->sy) || !coordinate(r->origin_x) || !coordinate(r->origin_y) || r->clip_count>16) return -E2BIG;
    /* Build the sole legal representation, so every unused field is checked. */
    struct ciuki_batch_record v={.opcode=r->opcode,.record_bytes=128};
    if (r->opcode!=CIUKI_CMD_SYNC) {
        v.dst=r->dst;
        if (!c->dst) return -EINVAL;
        int e=swdib_target_valid(c->dst); if (e) return e;
        if (!(c->dst->rights&PROT_WRITE)) return -EACCES;
    }
    if (r->opcode<=CIUKI_CMD_LINE) {
        v.dx=r->dx; v.dy=r->dy; v.clip_offset=r->clip_offset; v.clip_count=r->clip_count;
        if (r->opcode!=CIUKI_CMD_LINE) { v.dw=r->dw; v.dh=r->dh; }
        if (r->clip_count && !c->clips) return -EINVAL;
        for (unsigned i=0;i<r->clip_count;i++) {
            const struct ciuki_batch_clip *p=&c->clips[i];
            if (p->width>16384 || p->height>16384 || !coordinate(p->x) || !coordinate(p->y)) return -E2BIG;
        }
    }
    switch (r->opcode) {
    case CIUKI_CMD_COPY: case CIUKI_CMD_KEYED: case CIUKI_CMD_SCALED:
        v.src=r->src; v.sx=r->sx; v.sy=r->sy;
        if (!c->src) return -EINVAL;
        { int e=swdib_target_valid(c->src); if (e) return e; }
        if (!(c->src->rights&PROT_READ)) return -EACCES;
        if (r->opcode!=CIUKI_CMD_SCALED && !same_layout(&c->dst->device,&c->src->device)) return -EOPNOTSUPP;
        if (r->opcode==CIUKI_CMD_KEYED) v.key=r->key;
        if (r->opcode==CIUKI_CMD_SCALED) {
            v.sw=r->sw; v.sh=r->sh; v.filter=r->filter;
            if (r->filter>CIUKI_FILTER_NEAREST_EDGE) return -EOPNOTSUPP;
            if ((r->dw && r->dh && (!r->sw || !r->sh)) || r->sx<0 || r->sy<0 ||
                (uint64_t)(uint32_t)r->sx+r->sw>c->src->device.width || (uint64_t)(uint32_t)r->sy+r->sh>c->src->device.height) return -EINVAL;
        }
        if (c->src->identity==c->dst->identity) {
            unsigned sw=r->opcode==CIUKI_CMD_SCALED ? r->sw : r->dw, sh=r->opcode==CIUKI_CMD_SCALED ? r->sh : r->dh;
            if (rectangles_overlap(c->src,r->sx,r->sy,sw,sh,c->dst,r->dx,r->dy,r->dw,r->dh)) {
                if (r->opcode!=CIUKI_CMD_COPY || c->dst->offset!=c->src->offset || c->dst->device.pitch!=c->src->device.pitch) return -EINVAL;
                if (!(c->dst->rights&PROT_READ)) return -EACCES;
            }
        }
        break;
    case CIUKI_CMD_MONO: v.flags=r->flags; v.foreground=r->foreground; v.background=r->background;
        if (r->flags&~CIUKI_CMD_TRANSPARENT_BG) return -EINVAL;
        /* fall through */
    case CIUKI_CMD_WRITE:
        v.sx=r->sx; v.sy=r->sy; v.sw=r->sw; v.sh=r->sh;
        v.data_offset=r->data_offset; v.data_bytes=r->data_bytes; v.stride=r->stride;
        if (r->dw && r->dh) {
            uint64_t row=r->opcode==CIUKI_CMD_MONO ? (r->sw+7)/8 : (uint64_t)r->sw*4;
            if (r->sh && (uint64_t)(r->sh-1)*r->stride+row>UINT32_MAX) return -EOVERFLOW;
            if (!r->sw || !r->sh || r->stride<row || (r->opcode==CIUKI_CMD_WRITE && (r->stride&3)) ||
                (uint64_t)(r->sh-1)*r->stride+row!=r->data_bytes || !c->data) return -EINVAL;
        } else if (r->data_offset || r->data_bytes || r->stride) return -EINVAL;
        break;
    case CIUKI_CMD_INVERT:
        if (!(c->dst->rights&PROT_READ)) return -EACCES;
        break;
    case CIUKI_CMD_PATTERN:
        v.background=r->background; v.origin_x=r->origin_x; v.origin_y=r->origin_y;
        memcpy(v.pattern,r->pattern,8);
        /* fall through */
    case CIUKI_CMD_FILL: v.foreground=r->foreground; break;
    case CIUKI_CMD_LINE: v.foreground=r->foreground; v.sx=r->sx; v.sy=r->sy; break;
    case CIUKI_CMD_CURSOR_MONO_SET: case CIUKI_CMD_CURSOR_ARGB_SET:
        v.dw=r->dw; v.dh=r->dh; v.origin_x=r->origin_x; v.origin_y=r->origin_y;
        v.data_offset=r->data_offset; v.data_bytes=r->data_bytes; v.stride=r->stride;
        if (!r->dw || !r->dh || r->dw>64 || r->dh>64 || r->origin_x<0 || r->origin_y<0 || (uint32_t)r->origin_x>=r->dw || (uint32_t)r->origin_y>=r->dh || !c->data) return -EINVAL;
        if (r->opcode==CIUKI_CMD_CURSOR_MONO_SET) {
            v.foreground=r->foreground; v.background=r->background;
            if (r->dw!=32 || r->dh!=32 || r->stride!=4 || r->data_bytes!=256) return -EINVAL;
        } else {
            if ((uint64_t)(r->dh-1)*r->stride+r->dw*4>UINT32_MAX) return -EOVERFLOW;
            if (r->stride<r->dw*4 || (r->stride&3) || (uint64_t)(r->dh-1)*r->stride+r->dw*4!=r->data_bytes) return -EINVAL;
            for (unsigned y=0;y<r->dh;y++) for (unsigned x=0;x<r->dw;x++) {
                const uint8_t *p=c->data+y*r->stride+x*4;
                if (p[0]>p[3] || p[1]>p[3] || p[2]>p[3]) return -EINVAL;
            }
        }
        break;
    case CIUKI_CMD_CURSOR_MOVE: v.dx=r->dx; v.dy=r->dy; break;
    case CIUKI_CMD_CURSOR_SHOW: v.flags=r->flags; if (r->flags&~CIUKI_CMD_SHOW) return -EINVAL; break;
    default: break;
    }
    if ((v.foreground|v.background|v.key)&0xff000000u || memcmp(r,&v,sizeof(v))) return -EINVAL;
    if (r->opcode>=CIUKI_CMD_CURSOR_MONO_SET && c->dst->kind!=CIUKI_TARGET_SCANOUT) return -EINVAL;
    return 0;
}
static bool eligible(const struct swdib_command *c, int64_t x, int64_t y)
{
    if (x<0 || y<0 || x>=c->dst->device.width || y>=c->dst->device.height) return false;
    if (!c->record->clip_count) return true;
    for (unsigned i=0;i<c->record->clip_count;i++) {
        const struct ciuki_batch_clip *p=&c->clips[i];
        if (x>=p->x && y>=p->y && x<(int64_t)p->x+p->width && y<(int64_t)p->y+p->height) return true;
    }
    return false;
}
static int64_t floor_div(int64_t x, int64_t n)
{ int64_t q=x/n; return q-(x%n<0); }
static unsigned clamp_index(int64_t x, unsigned size)
{ return x<0 ? 0 : x>=size ? size-1 : (unsigned)x; }
static uint32_t edge_pixel(const struct swdib_command *c,unsigned i,unsigned j)
{
    const struct ciuki_batch_record *r=c->record;
    return load_pixel(&c->src->device,r->sx+(uint64_t)i*r->sw/r->dw,r->sy+(uint64_t)j*r->sh/r->dh);
}
static uint32_t scaled(const struct swdib_command *c, unsigned i, unsigned j)
{
    const struct ciuki_batch_record *r=c->record; const struct fb_device *s=&c->src->device;
    if (r->filter==CIUKI_FILTER_NEAREST_EDGE)
        return decode(s,edge_pixel(c,i,j));
    if (!r->filter) return decode(s,load_pixel(s,r->sx+(uint64_t)(2*i+1)*r->sw/(2*r->dw),r->sy+(uint64_t)(2*j+1)*r->sh/(2*r->dh)));
    int64_t ax=2*r->dw, ay=2*r->dh, qx=(int64_t)(2*i+1)*r->sw-r->dw, qy=(int64_t)(2*j+1)*r->sh-r->dh;
    int64_t l=floor_div(qx,ax), t=floor_div(qy,ay), rx=qx-l*ax, ry=qy-t*ay;
    uint64_t weights[]={(ax-rx)*(ay-ry),rx*(ay-ry),(ax-rx)*ry,rx*ry};
    uint32_t rgb[4];
    for (unsigned n=0;n<4;n++) rgb[n]=decode(s,load_pixel(s,r->sx+clamp_index(l+(n&1),r->sw),r->sy+clamp_index(t+(n>>1),r->sh)));
    uint32_t out=0;
    for (unsigned shift=0;shift<24;shift+=8) {
        uint64_t sum=0; for (unsigned n=0;n<4;n++) sum+=((rgb[n]>>shift)&255)*weights[n];
        out|=(uint32_t)((sum+(uint64_t)ax*ay/2)/((uint64_t)ax*ay))<<shift;
    }
    return out;
}
/* Pure geometry, before admission: no pixel reads. A line's major coordinate
 * advances by one; its rounded minor coordinate is monotone with unit steps.
 * Thus the two endpoints of a clipped major interval prove intersection. */
bool swdib_footprint(const struct swdib_command *c)
{
    const struct ciuki_batch_record *r=c->record;
    for (unsigned clip=0;clip<(r->clip_count ? r->clip_count : 1);clip++) {
        int64_t l=0,t=0,right=c->dst->device.width,bottom=c->dst->device.height;
        if (r->clip_count) {
            const struct ciuki_batch_clip *p=&c->clips[clip];
            if (p->x>l) l=p->x;
            if (p->y>t) t=p->y;
            if ((int64_t)p->x+p->width<right) right=(int64_t)p->x+p->width;
            if ((int64_t)p->y+p->height<bottom) bottom=(int64_t)p->y+p->height;
        }
        if (l>=right || t>=bottom) continue;
        if (r->opcode==CIUKI_CMD_LINE) {
            int64_t vx=(int64_t)r->sx-r->dx,vy=(int64_t)r->sy-r->dy,ax=vx<0 ? -vx : vx,ay=vy<0 ? -vy : vy;
            bool major_x=ax>=ay; int64_t n=major_x ? ax : ay;
            if (!n) continue;
            int64_t a=major_x ? r->dx : r->dy, delta=major_x ? vx : vy;
            int64_t lo=major_x ? l : t, hi=major_x ? right : bottom;
            int64_t first=delta<0 ? a-hi+1 : lo-a, end=delta<0 ? a-lo+1 : hi-a;
            if (first<0) first=0;
            if (end>n) end=n;
            if (first>=end) continue;
            int64_t minor=major_x ? r->dy : r->dx, slope=major_x ? vy : vx;
            int64_t u=floor_div(2*(minor*n+slope*first)+n,2*n), v=floor_div(2*(minor*n+slope*(end-1))+n,2*n);
            if (u>v) { int64_t swap=u;u=v;v=swap; }
            if (u<(major_x ? bottom : right) && v>=(major_x ? t : l)) return true;
            continue;
        }
        int64_t dx=r->dx,dy=r->dy,dr=dx+r->dw,db=dy+r->dh;
        if (dx>l) l=dx;
        if (dy>t) t=dy;
        if (dr<right) right=dr;
        if (db<bottom) bottom=db;
        if (r->opcode==CIUKI_CMD_COPY || r->opcode==CIUKI_CMD_KEYED || r->opcode==CIUKI_CMD_WRITE || r->opcode==CIUKI_CMD_MONO) {
            int64_t sw=c->src ? c->src->device.width : r->sw, sh=c->src ? c->src->device.height : r->sh;
            if (dx-r->sx>l) l=dx-r->sx;
            if (dy-r->sy>t) t=dy-r->sy;
            if (dx-r->sx+sw<right) right=dx-r->sx+sw;
            if (dy-r->sy+sh<bottom) bottom=dy-r->sy+sh;
        }
        if (l<right && t<bottom) return true;
    }
    return false;
}
void swdib_execute(const struct swdib_command *c, void (*pause)(void *), void *arg)
{
    const struct ciuki_batch_record *r=c->record;
    if (r->opcode>=CIUKI_CMD_SYNC) return;
    const struct fb_device *d=&c->dst->device;
    unsigned budget=0;
    if (r->opcode==CIUKI_CMD_LINE) {
        int64_t vx=(int64_t)r->sx-r->dx, vy=(int64_t)r->sy-r->dy;
        int64_t ax=vx<0 ? -vx : vx, ay=vy<0 ? -vy : vy, n=ax>=ay ? ax : ay;
        for (int64_t k=0;k<n;k++) {
            int64_t x=ax>=ay ? r->dx+(vx<0 ? -k : k) : floor_div(2*((int64_t)r->dx*n+vx*k)+n,2*n);
            int64_t y=ax>=ay ? floor_div(2*((int64_t)r->dy*n+vy*k)+n,2*n) : r->dy+(vy<0 ? -k : k);
            if (eligible(c,x,y)) store_pixel(d,x,y,swdib_pack(d,r->foreground));
            if (++budget==8) { row_written(); if (pause) pause(arg); budget=0; }
        }
        row_written(); return;
    }
    bool backwards=r->opcode==CIUKI_CMD_COPY && c->src->identity==c->dst->identity && (r->dy>r->sy || (r->dy==r->sy && r->dx>r->sx));
    for (unsigned row=0;row<r->dh;row++) for (unsigned col=0;col<r->dw;col++) {
        unsigned i=backwards ? r->dw-1-col : col, j=backwards ? r->dh-1-row : row;
        int64_t x=(int64_t)r->dx+i, y=(int64_t)r->dy+j, u=(int64_t)r->sx+i, v=(int64_t)r->sy+j;
        if (eligible(c,x,y)) {
            uint32_t value=0; bool write=true;
            switch (r->opcode) {
            case CIUKI_CMD_FILL: value=swdib_pack(d,r->foreground); break;
            case CIUKI_CMD_PATTERN:
                value=swdib_pack(d,(r->pattern[(unsigned)(y-r->origin_y)&7]&(128u>>((unsigned)(x-r->origin_x)&7))) ? r->foreground : r->background); break;
            case CIUKI_CMD_INVERT: value=load_pixel(d,x,y)^rgb_mask(d); break;
            case CIUKI_CMD_COPY: case CIUKI_CMD_KEYED:
                if (u<0 || v<0 || u>=c->src->device.width || v>=c->src->device.height) { write=false; break; }
                value=load_pixel(&c->src->device,u,v)&rgb_mask(d);
                if (r->opcode==CIUKI_CMD_KEYED && value==swdib_pack(d,r->key)) write=false;
                break;
            case CIUKI_CMD_WRITE: case CIUKI_CMD_MONO:
                if (u<0 || v<0 || u>=r->sw || v>=r->sh) { write=false; break; }
                if (r->opcode==CIUKI_CMD_WRITE) {
                    const uint8_t *p=c->data+v*r->stride+u*4;
                    value=swdib_pack(d,(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16);
                } else {
                    bool bit=c->data[v*r->stride+u/8]&(128u>>(u%8));
                    if (!bit && (r->flags&CIUKI_CMD_TRANSPARENT_BG)) write=false;
                    value=swdib_pack(d,bit ? r->foreground : r->background);
                }
                break;
            case CIUKI_CMD_SCALED:
                /* Edge stretch is also an exact packed copy for identical
                 * formats (including ignored X bits), like ordinary COPY. */
                if (r->filter==CIUKI_FILTER_NEAREST_EDGE && same_layout(d,&c->src->device))
                    value=edge_pixel(c,i,j);
                else value=swdib_pack(d,scaled(c,i,j));
                break;
            default: write=false; break;
            }
            if (write) store_pixel(d,x,y,value);
        }
        if (++budget==8) { row_written(); if (pause) pause(arg); budget=0; }
    }
    row_written();
}
