/* 101-b synthetic DOS frames through the production presenter and surface
 * writer. No guest game data. SPDX-License-Identifier: GPL-2.0-only */
#include <time.h>
static uint64_t video101_ns(void)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static void video101_budget(const char *name, unsigned value, unsigned limit)
{
    printf("101-video %s=%u limit=%u %s\n", name, value, limit, value<=limit ? "PASS" : "FAIL");
    fflush(stdout);
    if (!getenv("CIUKI_101_BASELINE")) CHECK(value<=limit);
}
static void video101_palette(struct dosvm *v, unsigned index, unsigned red)
{
    port_out(v,0x3c8,1,index);
    port_out(v,0x3c9,1,red); port_out(v,0x3c9,1,0); port_out(v,0x3c9,1,0);
}
static void video101_reference(struct dosvm *v, const void *pixels, uint64_t tick)
{
    struct ciuki_rect d;
    v->video_presented=false;
    CHECK(dosvm_vga_present(v,capture_row,reference_buffer,tick,&d)==1);
    CHECK(!memcmp(pixels,reference_buffer,sizeof(reference_buffer)));
}
static void test_101_video(void)
{
    static uint32_t pixels[640*480];
    struct surface surface={.info={.allocation_bytes=sizeof(pixels),.width=640,.height=480,.stride=2560},
                            .pixels=(uint8_t *)pixels,.references=2};
    struct ciuki_rect d;
    struct dosvm *v=create(); int10(v,0x13,0,0,0);
    struct row_target target={&surface,true,v};
    for (unsigned at=0;at<64000;at++) linear_store(v,at,5);
    CHECK(dosvm_vga_present(v,put_row,&target,0,&d)==1); target.force=false;
    unsigned converted=v->perf.converted_bytes, reads=surface_reads;
    video101_palette(v,200,31); /* colour absent from the displayed image */
    CHECK(dosvm_vga_present(v,put_row,&target,40,&d)==0);
    video101_budget("13h_unused_palette_converted",v->perf.converted_bytes-converted,0);
    video101_budget("13h_unused_palette_surface_reads",surface_reads-reads,0);
    video101_reference(v,pixels,80);
    converted=v->perf.converted_bytes; reads=surface_reads;
    video101_palette(v,5,31);
    CHECK(dosvm_vga_present(v,put_row,&target,120,&d)==1 && d.dst_y==40 && d.height==400);
    video101_budget("13h_fade_converted",v->perf.converted_bytes-converted,64000);
    video101_budget("13h_fade_surface_reads",surface_reads-reads,0);
    video101_reference(v,pixels,160);
    /* A subsequent unused change and a same-value write do no work. */
    converted=v->perf.converted_bytes; reads=surface_reads;
    video101_palette(v,200,30); video101_palette(v,5,31);
    CHECK(dosvm_vga_present(v,put_row,&target,200,&d)==0);
    video101_budget("13h_redundant_palette_converted",v->perf.converted_bytes-converted,0);
    video101_budget("13h_redundant_palette_reads",surface_reads-reads,0);
    destroy(v,true); empty();

    v=create(); video_stage(v,4); target.vm=v; target.force=true;
    dosvm_vga_sync(v,true);
    /* Equal Mode X pages: a CRTC change is not changed pixels. */
    for (unsigned p=0;p<4;p++) memset(v->vga.plane[p],5,CVGA_PLANE_SIZE);
    v->vga.changes++;
    CHECK(dosvm_vga_present(v,put_row,&target,0,&d)==1); target.force=false;
    port_out(v,0x3d4,2,0x800c); port_out(v,0x3d4,2,0x000d);
    converted=v->perf.converted_bytes;
    CHECK(dosvm_vga_present(v,put_row,&target,40,&d)==0);
    video101_budget("modex_equal_flip_converted",v->perf.converted_bytes-converted,0);
    video101_palette(v,5,31); reads=surface_reads;
    CHECK(dosvm_vga_present(v,put_row,&target,80,&d)==1);
    video101_budget("modex_fade_surface_reads",surface_reads-reads,0);
    video101_reference(v,pixels,120);
    destroy(v,true); empty();

    bool available=fs_vbe_available; fs_vbe_available=true;
    for (unsigned mode=0;mode<2;mode++) {
        CHECK(!dosvm_create(0,&v)); int10(v,0x4f02,mode ? 0x4114 : 0x4101,0,0);
        CHECK(v->vbe_mode && v->vbe_buffer.used);
        target.vm=v; target.force=true;
        CHECK(dosvm_vga_present(v,put_row,&target,0,&d)==1); target.force=false;
        converted=v->perf.converted_bytes; reads=surface_reads;
        video101_palette(v,200,31);
        CHECK(dosvm_vga_present(v,put_row,&target,40,&d)==0);
        video101_budget(mode ? "vbe16_palette_converted" : "vbe8_unused_palette_converted",
                        v->perf.converted_bytes-converted,0);
        video101_budget(mode ? "vbe16_palette_reads" : "vbe8_unused_palette_reads",surface_reads-reads,0);
        if (!mode) {
            reads=surface_reads; video101_palette(v,0,31);
            CHECK(dosvm_vga_present(v,put_row,&target,80,&d)==1);
            video101_budget("vbe8_fade_surface_reads",surface_reads-reads,0);
            video101_reference(v,pixels,120);
            /* BIOS block palettes obey the same cadence as DAC OUTs, and
             * do not invalidate the retained surface between entries. */
            uint8_t palette[4]={0,0,30,0};
            CHECK(!dosvm_write(v,0x20000,palette,sizeof(palette)));
            struct v86_frame f=vf(0x4f09,0,1,0); f.es=0x2000;
            CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004f);
            reads=surface_reads;
            int early=dosvm_vga_present(v,put_row,&target,120,&d); CHECK(early>=0);
            video101_budget("vbe8_bios_palette_early_frame",(unsigned)early,0);
            CHECK(dosvm_vga_present(v,put_row,&target,160,&d)==(early ? 0 : 1));
            video101_budget("vbe8_bios_palette_reads",surface_reads-reads,0);
            video101_reference(v,pixels,200);
            static uint8_t indexes[640*480]; memset(indexes,5,sizeof(indexes));
            CHECK(!dosvm_write(v,v->vbe_buffer.base,indexes,sizeof(indexes)));
            for (unsigned pg=0;pg<sizeof(indexes)/PAGE_SIZE;pg++) *vbe_pte(v,pg)|=0x40;
            reads=surface_reads;
            CHECK(dosvm_vga_present(v,put_row,&target,240,&d)==1 && d.height==480);
            video101_budget("vbe8_frame_surface_reads",surface_reads-reads,0);
            video101_reference(v,pixels,280);
            CHECK(!dosvm_write(v,v->vbe_buffer.base,indexes,sizeof(indexes)));
            for (unsigned pg=0;pg<sizeof(indexes)/PAGE_SIZE;pg++) *vbe_pte(v,pg)|=0x40;
            converted=v->perf.converted_bytes;
            CHECK(!dosvm_vga_present(v,put_row,&target,320,&d));
            video101_budget("vbe8_equal_frame_converted",v->perf.converted_bytes-converted,0);
            port_out(v,0x3c6,1,0);
            CHECK(dosvm_vga_present(v,put_row,&target,360,&d)==1);
            video101_reference(v,pixels,400);
            converted=v->perf.converted_bytes; video101_palette(v,5,1);
            CHECK(!dosvm_vga_present(v,put_row,&target,440,&d));
            video101_budget("vbe8_masked_palette_converted",v->perf.converted_bytes-converted,0);
        }
        destroy(v,true); empty();
    }
    fs_vbe_available=available;
    puts("101-video palette damage, mode 13h fade, Mode X flip, VBE8/16 pixels PASS");
}
/* Timing is descriptive only; the work-count budgets above are deterministic.
 * Includes surface read/compare/write, excludes synthetic guest RAM stores. */
static void bench_101_video(void)
{
    static uint32_t pixels[640*480];
    static uint8_t source[640*480];
    struct surface surface={.info={.allocation_bytes=sizeof(pixels),.width=640,.height=480,.stride=2560},
                            .pixels=(uint8_t *)pixels,.references=2};
    bool available=fs_vbe_available; fs_vbe_available=true;
    const unsigned frames=64;
    for (unsigned mode=0;mode<3;mode++) {
        struct dosvm *v; CHECK(!dosvm_create(0,&v));
        if (mode==0) {
            int10(v,0x13,0,0,0);
            for (unsigned at=0;at<64000;at++) linear_store(v,at,5);
        } else if (mode==1) {
            video_stage(v,4); dosvm_vga_sync(v,true);
            for (unsigned p=0;p<4;p++) {
                memset(v->vga.plane[p],5,CVGA_PLANE_SIZE);
                memset(v->vga.plane[p]+0x8000,6,19200);
            }
            v->vga.changes++;
        } else int10(v,0x4f02,0x4101,0,0);
        struct row_target target={&surface,true,v}; struct ciuki_rect d;
        CHECK(dosvm_vga_present(v,put_row,&target,0,&d)==1); target.force=false;
        uint64_t ns=0; unsigned reads=surface_reads, writes=surface_rows, converted=v->perf.converted_bytes;
        for (unsigned f=1;f<=frames;f++) {
            if (mode==0) video101_palette(v,5,f%63);
            else if (mode==1) port_out(v,0x3d4,2,(f&1) ? 0x800c : 0x000c);
            else {
                memset(source,(int)(f%63),sizeof(source));
                CHECK(!dosvm_write(v,v->vbe_buffer.base,source,sizeof(source)));
                for (unsigned pg=0;pg<sizeof(source)/PAGE_SIZE;pg++) *vbe_pte(v,pg)|=0x40;
            }
            uint64_t start=video101_ns();
            CHECK(dosvm_vga_present(v,put_row,&target,f*40,&d)>=0);
            ns+=video101_ns()-start;
        }
        printf("101-video bench=%s frames=%u ns_per_frame=%llu converted_per_frame=%u surface_reads=%u surface_writes=%u\n",
               mode==0 ? "13h-fade" : mode==1 ? "modex-flip" : "vbe8-frame",frames,
               (unsigned long long)(ns/frames),(v->perf.converted_bytes-converted)/frames,
               (surface_reads-reads)/frames,(surface_rows-writes)/frames);
        fflush(stdout); destroy(v,true); empty();
    }
    fs_vbe_available=available;
}
