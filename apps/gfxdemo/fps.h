/* Bounded statistics for completed frames. SPDX-License-Identifier: MIT */
#ifndef GFX_FPS_H
#define GFX_FPS_H
#include <stdint.h>
#include <stdlib.h>
#define GFX_FPS_SAMPLES 4096
struct gfx_fps {
    uint64_t begin,end,last,frames,total_ns;
    uint64_t intervals[GFX_FPS_SAMPLES];
    unsigned count,overflow;
};
static inline void gfx_fps_start(struct gfx_fps *s,uint64_t now,unsigned seconds)
{
    *s=(struct gfx_fps){.begin=now,.end=now+(uint64_t)seconds*1000000000ull,.last=now};
}
static inline void gfx_fps_frame(struct gfx_fps *s,uint64_t now)
{
    if (now<s->last || now>s->end) return;
    uint64_t interval=now-s->last;
    if (s->count<GFX_FPS_SAMPLES) s->intervals[s->count++]=interval;
    else s->overflow=1;
    s->frames++;s->total_ns+=interval;s->last=now;
}
static inline int gfx_fps_compare(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return x>y ? 1 : x<y ? -1 : 0;
}
static inline uint64_t gfx_fps_p95(struct gfx_fps *s)
{
    if (!s->count) return 0;
    qsort(s->intervals,s->count,sizeof(s->intervals[0]),gfx_fps_compare);
    return s->intervals[(s->count*95u+99u)/100u-1];
}
#endif
