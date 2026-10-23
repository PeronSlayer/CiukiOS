/* Production engine sequences, fault proofs and independent pixel goldens.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <limits.h>
#include <ciuki/swdib.h>
#include "cirrus_fake.h"

static void sequence(void)
{
    struct cirrus_fake f={0}; struct cirrus_engine s={0};
    struct fb_device fb=fake_fb(&f,24); struct cirrus_io io=fake_io(&f);
    assert(!cirrus_engine_start(&s,&fb,&io,0x00b8));
    assert(!f.trace_count); /* Initial idle proof submits nothing. */
    f.gr[0x31]=0x80; /* Firmware autostart must be disabled first. */
    struct fb_rect r={2,3,5,4};
    assert(!s.ops.fill_rect(&s,&r,0x123456));
    static const uint8_t indices[]={0x31,0,1,0x10,0x11,0x12,0x13,
        0x24,0x25,0x26,0x27,0x20,0x21,0x22,0x23,
        0x28,0x29,0x2a,0x2c,0x2d,0x2e,0x30,0x32,0x31};
    static const uint8_t values[]={0,0x56,0x56,0x34,0x34,0x12,0x12,
        104,0,104,0,14,0,3,0,0x3e,1,0,0,0,0,0xe0,0x0d,2};
    assert(f.trace_count==sizeof(indices) && f.starts==1 && f.drains==1);
    for (unsigned i=0;i<sizeof(indices);i++)
        assert(f.trace[i].index==indices[i] && f.trace[i].value==values[i]);
    f.trace_count=0;
    assert(!s.ops.copy_rect(&s,&r,4,5));
    assert(f.gr[0x30]==1 && fake_value(&f,0x2c,3)==644 && fake_value(&f,0x28,3)==858);
    assert(fake_value(&f,0x20,2)==14 && fake_value(&f,0x22,2)==3);
    assert(!s.ops.copy_rect(&s,&(struct fb_rect){4,5,5,4},2,3));
    assert(f.gr[0x30]==0 && fake_value(&f,0x2c,3)==532 && fake_value(&f,0x28,3)==318);
    unsigned writes=f.trace_count;
    assert(!s.ops.sync(&s) && f.trace_count==writes && s.cpu_safe);
    assert(!s.ops.cursor_set && !s.ops.cursor_move && !s.ops.cursor_show && s.ops.capabilities(&s)==1);
}
static void goldens(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=8) {
        struct cirrus_fake f={0}; struct cirrus_engine s={0};
        struct fb_device fb=fake_fb(&f,bpp); struct cirrus_io io=fake_io(&f);
        uint8_t expected[sizeof(f.pixels)];
        for (unsigned i=0;i<sizeof(expected);i++) f.pixels[i]=(uint8_t)(i*37+11);
        memcpy(expected,f.pixels,sizeof(expected));
        struct fb_device ref=fb; ref.mapped=expected;
        assert(!cirrus_engine_start(&s,&fb,&io,0xb8));
        struct fb_rect r={3,2,21,17};
        uint32_t colour=swdib_pack(&fb,0x12c456);
        swdib_fill(&ref,&r,colour); assert(!s.ops.fill_rect(&s,&r,colour));
        assert(!memcmp(expected,f.pixels,sizeof(expected)));
        /* Horizontal, vertical and diagonal overlap in both directions,
         * identical rectangles, disjoint copies and framebuffer corners. */
        static const int moves[][4]={{1,1,2,1},{2,1,1,1},{1,1,1,2},{1,2,1,1},
            {1,1,2,2},{2,2,1,1},{2,1,1,2},{1,2,2,1},
            {2,2,2,2},{0,0,20,12},{20,12,0,0}};
        for (unsigned k=0;k<ARRAY_SIZE(moves);k++) {
            for (unsigned i=0;i<sizeof(expected);i++) f.pixels[i]=(uint8_t)(i*71+k);
            memcpy(expected,f.pixels,sizeof(expected));
            struct fb_rect src={moves[k][0],moves[k][1],12,12};
            swdib_copy(&ref,&src,moves[k][2],moves[k][3]);
            assert(!s.ops.copy_rect(&s,&src,moves[k][2],moves[k][3]));
            assert(!memcmp(expected,f.pixels,sizeof(expected)));
        }
        assert(!s.ops.stop(&s) && !s.ops.capabilities(&s) && s.ops.cpu_ready(&s));
    }
}
static void refusal(void)
{
    struct cirrus_fake f={0}; struct cirrus_engine s={0};
    struct fb_device fb=fake_fb(&f,32); struct cirrus_io io=fake_io(&f);
    assert(!cirrus_engine_start(&s,&fb,&io,0xb8));
    struct fb_rect bad[]={{-1,0,1,1},{0,-1,1,1},{0,0,0,1},{0,0,1,0},
        {0,0,INT_MAX,1},{INT_MAX,0,INT_MAX,1},{31,23,2,2}};
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        assert(s.ops.fill_rect(&s,&bad[i],1)==-EOPNOTSUPP);
        assert(s.ops.copy_rect(&s,&bad[i],0,0)==-EOPNOTSUPP);
    }
    assert(s.ops.copy_rect(&s,&(struct fb_rect){0,0,4,4},INT_MAX,1)==-EOPNOTSUPP);
    assert(!f.trace_count && !f.starts);
    fb.pitch=8192; assert(cirrus_engine_start(&s,&fb,&io,0xb8)==-EINVAL);
    fb=fake_fb(&f,32); assert(cirrus_engine_start(&s,&fb,&io,0xa0)==-EOPNOTSUPP);
    for (unsigned i=0;i<3;i++) {
        static const uint16_t ids[]={0xa0,0xa4,0xac};
        fb=fake_fb(&f,24); memset(&s,0,sizeof(s));
        assert(!cirrus_engine_start(&s,&fb,&io,ids[i]));
        assert(s.max_width==2048 && s.max_height==1024);
        s.fb.width=1024; s.fb.pitch=4095;
        assert(s.ops.fill_rect(&s,&(struct fb_rect){0,0,683,1},0)==-EOPNOTSUPP);
    }
}
static int pauses;
static int pause_wait(void *ctx) { (void)ctx; pauses++; return 0; }
static int cancel_wait(void *ctx) { (void)ctx; return -EINTR; }
static void faults(void)
{
    struct cirrus_fake f={.step=100}; struct cirrus_engine s={0};
    struct fb_device fb=fake_fb(&f,16); struct cirrus_io io=fake_io(&f);
    assert(!cirrus_engine_start(&s,&fb,&io,0xb8));
    f.delay_reads=30; s.ops.set_pause(&s,pause_wait,NULL);
    assert(!s.ops.fill_rect(&s,&(struct fb_rect){0,0,2,2},5) && pauses>=2);
    f.stuck=true; f.partial=true; f.reset_fails=true;
    unsigned before=f.starts;
    assert(s.ops.copy_rect(&s,&(struct fb_rect){0,0,2,2},1,1)==-ETIMEDOUT);
    assert(f.starts==before+1 && s.quarantined && !s.cpu_safe && !s.ops.capabilities(&s));
    unsigned starts=f.starts;
    assert(s.ops.fill_rect(&s,&(struct fb_rect){0,0,2,2},5)==-EIO && f.starts==starts);
    assert(s.ops.stop(&s) && !s.cpu_safe);
    assert(cirrus_engine_start(&s,&fb,&io,0xb8)==-EINVAL); /* Same boot stays quarantined. */
    f.reset_fails=false;
    assert(!s.ops.sync(&s) && s.cpu_safe && s.quarantined && !s.active);
    assert(f.resets>=2);
    memset(&s,0,sizeof(s)); memset(&f,0,sizeof(f));
    fb=fake_fb(&f,16); io=fake_io(&f);
    f.stuck=true; f.gr[0x31]=3; f.reset_fails=true; /* Frozen clock. */
    assert(cirrus_engine_start(&s,&fb,&io,0xb8)==-ETIMEDOUT);
    assert(f.reads<=2*CIRRUS_WAIT_POLLS+1 && s.quarantined && !s.cpu_safe);
    f.absent=true; unsigned writes=f.trace_count;
    assert(s.ops.sync(&s)==-EIO && f.trace_count==writes && !s.cpu_safe);
    memset(&s,0,sizeof(s)); memset(&f,0,sizeof(f));
    fb=fake_fb(&f,24); io=fake_io(&f);
    assert(!cirrus_engine_start(&s,&fb,&io,0xb8));
    f.invalid=true;
    assert(s.ops.sync(&s)==-EIO && !s.cpu_safe && s.quarantined && !f.trace_count);
    memset(&s,0,sizeof(s)); memset(&f,0,sizeof(f));
    fb=fake_fb(&f,32); io=fake_io(&f); f.step=100;
    assert(!cirrus_engine_start(&s,&fb,&io,0xb8));
    f.delay_reads=30; s.ops.set_pause(&s,cancel_wait,NULL);
    assert(s.ops.fill_rect(&s,&(struct fb_rect){0,0,2,2},0x123456)==-EINTR);
    assert(s.quarantined && !s.active && s.cpu_safe && f.resets==1);
}
int main(void)
{
    sequence(); goldens(); refusal(); faults();
    puts("cirrus engine: sequences, 3-depth goldens, refusals and quarantine PASS");
    return 0;
}
