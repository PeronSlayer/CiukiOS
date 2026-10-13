/* Bounded synchronous display batch; fakeable surfaces/clock for host tests.
 * Destination-byte throughput, software includes RAM drawing + CPU upload.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_GFX_BENCHMARK_H
#define CIUKI_GFX_BENCHMARK_H
#include <ciuki/abi.h>
#include <errno.h>
#include <string.h>
struct gfx_bench_target {
    void *ctx;
    uint32_t *pixels, width, height, bpp, generation;
    uint64_t (*now)(void *);
    int (*info)(void *, struct ciuki_accel_info *);
    int (*accel)(void *, const struct ciuki_accel_request *);
    int (*upload)(void *, const struct ciuki_rect *);
};
struct gfx_bench_result { uint64_t bytes, ns, operations; };
static inline int gfx_bench_sample(const struct gfx_bench_target *t, unsigned index,
                                   struct gfx_bench_result *out)
{
    memset(out,0,sizeof(*out));
    if (index>=6 || !t || !t->now || !t->width || !t->height ||
        t->width>1600 || t->height>1200 || (t->bpp!=16 && t->bpp!=24 && t->bpp!=32)) return -EINVAL;
    unsigned op=index%3; int hw=index<3;
    if (op==1 && (t->width<512 || t->height<256)) return -EOPNOTSUPP;
    struct ciuki_accel_info before={0}, after={0};
    if (hw) {
        if (!t->info || !t->accel) return -ENODEV;
        int e=t->info(t->ctx,&before); if (e) return e;
        if (!(before.capabilities&1) || before.quarantined) return -ENODEV;
    } else if (!t->pixels || !t->upload) return -EINVAL;
    struct ciuki_accel_request q={.size=sizeof(q),.generation=t->generation,
        .operation=op==0 ? CIUKI_ACCEL_FILL : op==1 ? CIUKI_ACCEL_COPY : CIUKI_ACCEL_PATTERN,
        .colour=0x527e99,.background=0x172b40,
        .rect={0,0,op==1 ? 256 : 0,0,op==1 ? 256 : t->width,op==1 ? 256 : t->height},
        .pattern={0xaa,0x55,0xaa,0x55,0xaa,0x55,0xaa,0x55}};
    uint64_t start=t->now(t->ctx),end=start;
    unsigned iterations=0;
    int e=0;
    do {
        if (hw) e=t->accel(t->ctx,&q);
        else {
            if (op==1) {
                for (unsigned y=0;y<256;y++)
                    memmove(t->pixels+(size_t)y*t->width+256,t->pixels+(size_t)y*t->width,256*4);
            } else {
                for (unsigned y=0;y<t->height;y++) for (unsigned x=0;x<t->width;x++)
                    t->pixels[(size_t)y*t->width+x]=op==0 || (q.pattern[y&7]&(0x80u>>(x&7))) ? q.colour : q.background;
            }
            struct ciuki_rect r=q.rect; r.src_x=r.dst_x; r.src_y=r.dst_y;
            e=t->upload(t->ctx,&r);
        }
        if (e) return e;
        iterations++; end=t->now(t->ctx);
        if (end<start) return -EAGAIN;
    } while (end-start<8000000ull && iterations<64);
    if (end-start<8000000ull) return -EAGAIN; /* stopped/insufficient-resolution clock */
    if (hw) {
        e=t->info(t->ctx,&after); if (e) return e;
        if (after.generation!=before.generation || after.quarantined ||
            !(after.capabilities&1) || after.operations-before.operations!=iterations ||
            after.fallbacks!=before.fallbacks) return -EIO;
        out->operations=after.operations-before.operations;
    }
    out->bytes=(uint64_t)q.rect.width*q.rect.height*(t->bpp/8)*iterations;
    out->ns=end-start;
    return 0;
}
#endif
