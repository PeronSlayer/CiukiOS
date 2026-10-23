/* Literal Part C goldens plus the production registry/presenter fake adapter.
 * SPDX-License-Identifier: GPL-2.0-only */
#define DISPLAY_DRIVER_NO_MAIN
#include "display_driver_test.c"
#undef main
#include <assert.h>
static uint8_t dest_store[8192], source_store[8192], pristine[8192];
static unsigned vectors;
static struct swdib_target destination, source;
static uint32_t get(const struct swdib_target *t, unsigned x, unsigned y)
{
    const struct fb_device *d=&t->device; const uint8_t *p=d->mapped+y*d->pitch+x*(d->bpp/8);
    unsigned value=0; for (unsigned i=0;i<d->bpp/8;i++) value|=(unsigned)p[i]<<(i*8); return value;
}
static void raw(const struct swdib_target *t, unsigned x, unsigned y, uint32_t value)
{
    uint8_t *p=t->device.mapped+y*t->device.pitch+x*(t->device.bpp/8);
    for (unsigned i=0;i<t->device.bpp/8;i++) p[i]=(uint8_t)(value>>(i*8));
}
static void rgb(const struct swdib_target *t, unsigned x, unsigned y, uint32_t value)
{ raw(t,x,y,swdib_pack(&t->device,value)); }
static void expect(unsigned x, unsigned y, uint32_t value)
{ CHECK(get(&destination,x,y)==swdib_pack(&destination.device,value)); vectors++; }
static void guards(void)
{
    unsigned bytes=destination.device.bpp/8;
    for (unsigned i=0;i<sizeof(dest_store);i++) {
        bool pixel=i>=13 && i<13+destination.device.pitch*destination.device.height &&
            (i-13)%destination.device.pitch<destination.device.width*bytes;
        if (!pixel) CHECK(dest_store[i]==pristine[i]);
    }
}
static void reset_targets(unsigned bpp)
{
    memset(dest_store,0xa7,sizeof(dest_store)); memcpy(pristine,dest_store,sizeof(pristine));
    memset(source_store,0x6b,sizeof(source_store));
    destination=(struct swdib_target){.identity=dest_store,.rights=3,.kind=3,.device={.width=16,.height=16,
        .pitch=16*(bpp/8)+3,.size=16*(16*(bpp/8)+3),.mapped=dest_store+13,.bpp=bpp,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8}};
    source=destination; source.identity=source_store; source.device.mapped=source_store+11;
    CHECK(!swdib_target_valid(&destination));
    for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) rgb(&destination,x,y,0);
}
static void draw(struct ciuki_batch_record r, const void *data, const struct ciuki_batch_clip *clips)
{
    r.record_bytes=128; r.dst=1;
    struct swdib_command c={&r,&destination,r.src ? &source : NULL,clips,data};
    CHECK(!swdib_validate(&c));
    bool footprint=r.opcode==CIUKI_CMD_LINE && swdib_footprint(&c);
    swdib_execute(&c,NULL,NULL); guards();
    if (r.opcode==CIUKI_CMD_LINE) {
        bool any=false;
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) if (get(&destination,x,y)) any=true;
        CHECK(footprint==any);
    }
}
static void goldens(unsigned bpp)
{
    reset_targets(bpp);
    draw((struct ciuki_batch_record){.opcode=1,.dw=1,.dh=1,.foreground=0x123456},NULL,NULL);
    CHECK(get(&destination,0,0)==(bpp==16 ? 0x11aa : 0x123456));
    draw((struct ciuki_batch_record){.opcode=1,.dw=1,.dh=1,.foreground=0x070307},NULL,NULL);
    CHECK(get(&destination,0,0)==(bpp==16 ? 0 : 0x070307));
    draw((struct ciuki_batch_record){.opcode=1,.dw=1,.dh=1,.foreground=0x080408},NULL,NULL);
    CHECK(get(&destination,0,0)==(bpp==16 ? 0x821 : 0x080408));
    draw((struct ciuki_batch_record){.opcode=1,.dx=-1,.dw=2,.dh=2,.foreground=0xff0000},NULL,NULL);
    expect(0,0,0xff0000); expect(0,1,0xff0000); expect(1,0,0);
    uint8_t mono[]={0x81,0x80};
    draw((struct ciuki_batch_record){.opcode=5,.dw=10,.dh=1,.sw=10,.sh=1,.stride=2,.data_offset=8,.data_bytes=2,.foreground=0xff0000,.background=255},mono,NULL);
    for (unsigned i=0;i<10;i++) expect(i,0,(i==0 || i==7 || i==8) ? 0xff0000 : 255);
    for (unsigned i=0;i<3;i++) rgb(&destination,i,0,0x00ff00);
    draw((struct ciuki_batch_record){.opcode=5,.flags=1,.dx=-7,.dw=10,.dh=1,.sw=10,.sh=1,.stride=2,.data_offset=8,.data_bytes=2,.foreground=0xff0000},mono,NULL);
    expect(0,0,0xff0000); expect(1,0,0xff0000); expect(2,0,0x00ff00);
    uint8_t padded[]={0x80,0x7e,0x40};
    draw((struct ciuki_batch_record){.opcode=5,.dw=2,.dh=2,.sw=2,.sh=2,.stride=2,.data_offset=8,.data_bytes=3,.foreground=0xffffff},padded,NULL);
    expect(0,0,0xffffff); expect(1,0,0); expect(0,1,0); expect(1,1,0xffffff);
    struct ciuki_batch_record pattern={.opcode=3,.dw=3,.dh=2,.foreground=0xffffff,.pattern={0x80,0x40,0x20,0x10,8,4,2,1}};
    draw(pattern,NULL,NULL); expect(0,0,0xffffff); expect(1,0,0); expect(2,0,0); expect(0,1,0); expect(1,1,0xffffff);
    pattern.origin_x=pattern.origin_y=-1; draw(pattern,NULL,NULL); expect(0,0,0xffffff);
    rgb(&destination,0,0,0x123456);
    draw((struct ciuki_batch_record){.opcode=6,.dw=1,.dh=1},NULL,NULL);
    CHECK(get(&destination,0,0)==(bpp==16 ? 0xee55 : 0xedcba9));
    if (bpp==32) {
        raw(&destination,0,0,0xaa123456);
        draw((struct ciuki_batch_record){.opcode=6,.dw=1,.dh=1},NULL,NULL); CHECK(get(&destination,0,0)==0xaaedcba9);
        draw((struct ciuki_batch_record){.opcode=6,.dw=1,.dh=1},NULL,NULL); CHECK(get(&destination,0,0)==0xaa123456);
    }
    for (unsigned i=0;i<4;i++) rgb(&destination,i,0,0);
    struct ciuki_batch_clip clips[]={{0,0,3,1},{1,0,3,1},{1,0,3,1}};
    struct ciuki_batch_record invert={.opcode=6,.dw=4,.dh=1,.clip_count=3,.clip_offset=8};
    draw(invert,NULL,clips); for (unsigned i=0;i<4;i++) expect(i,0,0xffffff);
    draw(invert,NULL,clips); for (unsigned i=0;i<4;i++) expect(i,0,0);
    clips[0]=clips[1]=clips[2]=(struct ciuki_batch_clip){0}; draw(invert,NULL,clips); expect(0,0,0);
    uint32_t colours[]={0xff0000,0x00ff00,0x0000ff,0xffffff,0};
    for (unsigned i=0;i<5;i++) rgb(&destination,i,0,colours[i]);
    source=destination;
    struct ciuki_batch_clip union_clip[]={{3,0,2,1},{1,0,2,1}};
    draw((struct ciuki_batch_record){.opcode=2,.src=1,.dx=1,.dw=4,.dh=1,.clip_count=2,.clip_offset=8},NULL,union_clip);
    for (unsigned i=0;i<5;i++) expect(i,0,colours[i ? i-1 : 0]);
    for (unsigned i=0;i<5;i++) rgb(&destination,i,0,colours[i]);
    draw((struct ciuki_batch_record){.opcode=2,.src=1,.sx=1,.dw=4,.dh=1},NULL,NULL);
    for (unsigned i=0;i<5;i++) expect(i,0,colours[i<4 ? i+1 : 4]);
    for (unsigned direction=0;direction<2;direction++) {
        for (unsigned y=0;y<3;y++) for (unsigned x=0;x<2;x++) rgb(&destination,x,y,colours[(y*2+x)%5]);
        draw((struct ciuki_batch_record){.opcode=2,.src=1,.sy=direction ? 1 : 0,.dy=direction ? 0 : 1,.dw=2,.dh=2},NULL,NULL);
        for (unsigned y=0;y<2;y++) for (unsigned x=0;x<2;x++) expect(x,y+(direction ? 0 : 1),colours[((y+(direction ? 1 : 0))*2+x)%5]);
    }
    source.identity=source_store; source.device.mapped=source_store+11;
    rgb(&source,0,0,0xff0000); rgb(&source,1,0,255); rgb(&source,2,0,0xff00);
    for (unsigned i=0;i<3;i++) rgb(&destination,i,0,0xffffff);
    draw((struct ciuki_batch_record){.opcode=7,.src=1,.dw=3,.dh=1,.key=255},NULL,NULL);
    expect(0,0,0xff0000); expect(1,0,0xffffff); expect(2,0,0xff00);
    if (bpp==16) { rgb(&source,0,0,0xf8); draw((struct ciuki_batch_record){.opcode=7,.src=1,.dw=1,.dh=1,.key=255},NULL,NULL); expect(0,0,0xff0000); }
    if (bpp==32) {
        raw(&source,0,0,0xaa0000ff); raw(&destination,0,0,0xbb123456);
        draw((struct ciuki_batch_record){.opcode=7,.src=1,.dw=1,.dh=1,.key=255},NULL,NULL); CHECK(get(&destination,0,0)==0xbb123456);
        raw(&source,0,0,0xaa123456); draw((struct ciuki_batch_record){.opcode=2,.src=1,.dw=1,.dh=1},NULL,NULL); CHECK(get(&destination,0,0)==0x123456);
    }
    rgb(&source,0,0,0); rgb(&source,1,0,0xffffff);
    struct ciuki_batch_record scale={.opcode=8,.src=1,.sw=2,.sh=1,.dw=3,.dh=1};
    draw(scale,NULL,NULL); expect(0,0,0); expect(1,0,0xffffff); expect(2,0,0xffffff);
    scale.filter=1; draw(scale,NULL,NULL); expect(0,0,0); expect(1,0,0x808080); expect(2,0,0xffffff);
    scale.clip_count=1; scale.clip_offset=8; struct ciuki_batch_clip middle={1,0,1,1};
    rgb(&destination,0,0,255); draw(scale,NULL,&middle); expect(0,0,255); expect(1,0,0x808080);
    rgb(&source,0,0,0xff0000); rgb(&source,1,0,0xff00); rgb(&source,2,0,255);
    draw((struct ciuki_batch_record){.opcode=8,.src=1,.sw=3,.sh=1,.dw=2,.dh=1},NULL,NULL); expect(0,0,0xff0000); expect(1,0,255);
    rgb(&source,0,0,0); rgb(&source,1,0,0xff0000); rgb(&source,0,1,0xff00); rgb(&source,1,1,255);
    draw((struct ciuki_batch_record){.opcode=8,.src=1,.sw=2,.sh=2,.dw=1,.dh=1,.filter=1},NULL,NULL); expect(0,0,0x404040);
    struct fb_device *d=&destination.device;
    CHECK(swdib_cursor_argb(d,swdib_pack(d,255),0x80800000)==(bpp==16 ? 0x800f : 0x80007f));
    CHECK(swdib_cursor_argb(d,0xaa123456,0)==0xaa123456);
    CHECK(swdib_cursor_argb(d,0,0xffff0000)==swdib_pack(d,0xff0000));
    CHECK(swdib_cursor_argb(d,swdib_pack(d,0xff0000),0x80000080)==(bpp==16 ? 0x7810 : 0x7f0080));
    for (unsigned i=0;i<4;i++) CHECK(swdib_cursor_mono(d,swdib_pack(d,255),i/2,i%2,0xff0000,0xff00)==swdib_pack(d,(uint32_t[]){0xff00,0xff0000,255,0xffff00}[i]));
    uint8_t upload[]={0x56,0x34,0x12,0xaa};
    draw((struct ciuki_batch_record){.opcode=4,.dw=1,.dh=1,.sw=1,.sh=1,.data_offset=8,.data_bytes=4,.stride=4},upload,NULL); expect(0,0,0x123456);
    /* Literal line vectors L1-L6, original samples masked by target bounds. */
    const int endpoints[][4]={{0,0,4,2},{4,2,0,0},{0,0,2,4},{0,2,4,0},{-2,-1,2,1},{1,1,1,1},{2,3,2,0}};
    const int points[][8]={{0,0,1,1,2,1,3,2},{4,2,3,2,2,1,1,1},{0,0,1,1,1,2,2,3},{0,2,1,2,2,1,3,1},{0,0,1,1,-1,-1,-1,-1},{-1,-1,-1,-1,-1,-1,-1,-1},{2,3,2,2,2,1,-1,-1}};
    for (unsigned n=0;n<7;n++) {
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) rgb(&destination,x,y,0);
        draw((struct ciuki_batch_record){.opcode=9,.dx=endpoints[n][0],.dy=endpoints[n][1],.sx=endpoints[n][2],.sy=endpoints[n][3],.foreground=0xffffff},NULL,NULL);
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) {
            bool on=false; for (unsigned i=0;i<4;i++) if (points[n][2*i]==(int)x && points[n][2*i+1]==(int)y) on=true;
            expect(x,y,on ? 0xffffff : 0);
        }
    }
    /* All octants: reversal changes only the two endpoint inclusions, and
     * translation and clip permutations cannot rerasterize the line. */
    for (int vx=-4;vx<=4;vx++) for (int vy=-4;vy<=4;vy++) {
        uint8_t forward[16][16];
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) rgb(&destination,x,y,0);
        draw((struct ciuki_batch_record){.opcode=9,.dx=8,.dy=8,.sx=8+vx,.sy=8+vy,.foreground=0xffffff},NULL,NULL);
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) forward[y][x]=get(&destination,x,y)!=0;
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) rgb(&destination,x,y,0);
        draw((struct ciuki_batch_record){.opcode=9,.dx=8+vx,.dy=8+vy,.sx=8,.sy=8,.foreground=0xffffff},NULL,NULL);
        for (int y=0;y<16;y++) for (int x=0;x<16;x++) if ((x!=8 || y!=8) && (x!=8+vx || y!=8+vy)) CHECK((get(&destination,x,y)!=0)==forward[y][x]);
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) rgb(&destination,x,y,0);
        struct ciuki_batch_clip mask[]={{8,0,8,16},{8,0,8,16},{9,1,7,15}};
        draw((struct ciuki_batch_record){.opcode=9,.dx=9,.dy=9,.sx=9+vx,.sy=9+vy,.foreground=0xffffff,.clip_count=3,.clip_offset=8},NULL,mask);
        for (int y=1;y<16;y++) for (int x=1;x<16;x++) CHECK((get(&destination,x,y)!=0)==(x>=8 && forward[y-1][x-1]));
    }
    /* S6: decode source channels before repacking into another layout. */
    source.device.bpp=16; source.device.red_size=5; source.device.red_pos=11;
    source.device.green_size=6; source.device.green_pos=5; source.device.blue_size=5; source.device.blue_pos=0;
    raw(&source,0,0,0x8410);
    draw((struct ciuki_batch_record){.opcode=8,.src=1,.sw=1,.sh=1,.dw=1,.dh=1},NULL,NULL); expect(0,0,0x848284);
    struct swdib_target swapped=destination;
    destination.device.red_pos=0; destination.device.blue_pos=bpp==16 ? 11 : 16;
    draw((struct ciuki_batch_record){.opcode=1,.dw=1,.dh=1,.foreground=0x123456},NULL,NULL);
    CHECK(get(&destination,0,0)==(bpp==16 ? 0x51a2 : 0x563412)); destination=swapped;
    /* Invalid alpha and aliases are refused without touching backing. */
    uint8_t invalid_argb[]={0,255,0,128};
    struct ciuki_batch_record bad={.opcode=12,.record_bytes=128,.dst=1,.dw=1,.dh=1,.stride=4,.data_bytes=4,.data_offset=8};
    struct swdib_command command={&bad,&destination,NULL,NULL,invalid_argb};
    destination.kind=1; CHECK(swdib_validate(&command)==-EINVAL); destination.kind=3;
    bad=(struct ciuki_batch_record){.opcode=7,.record_bytes=128,.dst=1,.src=2,.dw=2,.dh=2};
    struct swdib_target alias=destination; command=(struct swdib_command){&bad,&destination,&alias,NULL,NULL};
    CHECK(swdib_validate(&command)==-EINVAL); alias.offset=1; bad.opcode=2; CHECK(swdib_validate(&command)==-EINVAL);
    /* Disjoint columns across several rows in one allocation are legal. */
    alias=destination; bad.opcode=7; bad.dx=4; CHECK(!swdib_validate(&command));
    if (bpp==16) {
        destination.device.red_pos=10; destination.device.green_size=5;
        draw((struct ciuki_batch_record){.opcode=1,.dw=1,.dh=1,.foreground=0xffffff},NULL,NULL); CHECK(get(&destination,0,0)==0x7fff);
        raw(&destination,0,0,0x9234); draw((struct ciuki_batch_record){.opcode=6,.dw=1,.dh=1},NULL,NULL); CHECK(get(&destination,0,0)==0xedcb);
        draw((struct ciuki_batch_record){.opcode=6,.dw=1,.dh=1},NULL,NULL); CHECK(get(&destination,0,0)==0x9234);
    }
    printf("commands %u bpp: P1-P4 C1-C5 M1-M3 T1-T2 I1-I4 K1-K2 S1-S6 L1-L6 W1 A1-A4, guards PASS\n",bpp);
}
struct wire { struct ciuki_batch_header h; struct ciuki_batch_record r[3]; struct ciuki_batch_target t; uint8_t payload[512]; };
static struct wire wire;
static struct display_batch job;
static struct accel_ops command_ops;
static unsigned native_calls, accepted;
static int cmd_error;
static unsigned error_at;
static void command_limits(void *ctx, uint32_t *w, uint32_t *h) { (void)ctx; *w=*h=64; }
static uint32_t command_bits(void *ctx) { (void)ctx; return 0x7ffe; }
static int command_native(void *ctx, const struct swdib_command *c)
{
    (void)ctx; native_calls++;
    CHECK(locks==1 && drawing==PRESENT_BUSY && accel_inflight);
    int error=error_at && native_calls!=error_at ? 0 : cmd_error;
    if (error==-EOPNOTSUPP) return error;
    accepted++;
    if (sliced && pause_fn) CHECK(!pause_fn(pause_ctx));
    if (error) { pixels[0]^=1; unsafe=true; return error; }
    swdib_execute(c,NULL,NULL); return 0;
}
static void make_job(unsigned count)
{
    memset(&wire,0,sizeof(wire)); memset(&job,0,sizeof(job));
    wire.h=(struct ciuki_batch_header){CIUKI_BATCH_MAGIC,1,64,64+128*count+32+512,count,64,64+128*count,1,64+128*count+32,512,0,42,{0}};
    struct ciuki_batch_target t={.kind=1,.generation=42}; memcpy((uint8_t *)&wire+wire.h.targets_offset,&t,32);
    for (unsigned i=0;i<count;i++) wire.r[i]=(struct ciuki_batch_record){.opcode=1,.record_bytes=128,.dst=1,.dw=4,.dh=1,.foreground=0xff0000};
    job.blob=(const void *)&wire;
    CHECK(!display_batch_header(&wire,wire.h.total_bytes,&job.header));
    job.targets[0]=(struct swdib_target){.device=device,.identity=&device,.rights=3,.kind=1};
    job.reply=(struct ciuki_batch_reply){.size=64,.version=1,.fence=1};
    native_calls=accepted=0; cmd_error=0; error_at=0;
    command_ops=fake_ops; command_ops.command=command_native; command_ops.command_bits=command_bits; command_ops.cursor_limits=command_limits;
    acceleration=&command_ops; batch_owner=NULL;
}
static void batch_tests(void)
{
    for (unsigned op=1;op<=14;op++) {
        if (op==10) continue;
        setup(); activate(); make_job(1);
        struct ciuki_batch_record *r=&wire.r[0]; r->opcode=op;
        if (op==2 || op==7 || op==8) { r->src=1; r->sx=8; }
        if (op==2 || op==6 || op==7 || op==8) r->foreground=0;
        if (op==8) { r->sw=4; r->sh=1; }
        if (op==4 || op==5 || op==11 || op==12) {
            r->sw=4; r->sh=1; r->stride=op==5 ? 1 : 16;
            r->data_offset=wire.h.payload_offset; r->data_bytes=op==5 ? 1 : 16;
            if (op==4) r->foreground=0;
            if (op>=11) { r->sw=r->sh=0; r->dw=r->dh=op==11 ? 32 : 1; r->stride=4; r->data_bytes=op==11 ? 256 : 4; if (op==12) r->foreground=0; }
        }
        if (op==9 || op==13 || op==14) { r->dw=r->dh=0; r->sx=op==9 ? 4 : 0; }
        if (op>=13) { r->foreground=0; batch_cursor_shape=true; }
        CHECK(!display_batch_validate(&job));
        CHECK(!fbdev_batch_execute(&job) && job.reply.completed==1 && job.reply.state==2 && (job.reply.flags&2));
        CHECK(native_calls==1 && accepted==1 && !drawing && !accel_inflight && !locks);
        uint8_t expected_pixels[sizeof(pixels)]; memcpy(expected_pixels,pixels,sizeof(pixels));
        struct ciuki_batch_record saved=*r;
        setup(); activate(); make_job(1); wire.r[0]=saved;
        if (op>=13) batch_cursor_shape=true;
        cmd_error=-EOPNOTSUPP;
        CHECK(!display_batch_validate(&job));
        if (op<=9) { CHECK(!fbdev_batch_execute(&job)); CHECK(!memcmp(expected_pixels,pixels,sizeof(pixels))); }
        else { memcpy(before,pixels,sizeof(pixels)); CHECK(fbdev_batch_execute(&job)==-EOPNOTSUPP); CHECK(!memcmp(before,pixels,sizeof(pixels))); }
        CHECK(!accepted && native_calls==1 && job.reply.state!=4);

    }
    setup(); activate(); make_job(1);
    wire.r[0]=(struct ciuki_batch_record){.opcode=5,.record_bytes=128,.dst=1,.dw=10,.dh=1,.sw=10,.sh=1,.foreground=0xff0000,.background=255,
        .data_offset=wire.h.payload_offset,.data_bytes=2,.stride=2};
    uint8_t *bits=(uint8_t *)&wire+wire.h.payload_offset; bits[0]=0x81; bits[1]=0x80;
    cmd_error=-EOPNOTSUPP; CHECK(!display_batch_validate(&job)); CHECK(!fbdev_batch_execute(&job));
    CHECK(!accepted && native_calls==1 && pixels[0]==0 && pixels[2]==255 && pixels[4]==255 && pixels[6]==0);
    setup(); activate(); make_job(2); batch_cursor_shape=false;
    wire.r[1]=(struct ciuki_batch_record){.opcode=CIUKI_CMD_CURSOR_MOVE,.record_bytes=128,.dst=1};
    memcpy(before,pixels,sizeof(pixels)); CHECK(!display_batch_validate(&job));
    CHECK(fbdev_batch_execute(&job)==-EINVAL && !job.reply.admitted && !native_calls && !memcmp(before,pixels,sizeof(pixels)));
    setup(); activate(); make_job(3); wire.r[1].opcode=6; wire.r[1].foreground=0; wire.r[2].reserved[0]=1;
    memcpy(before,pixels,sizeof(pixels)); CHECK(display_batch_validate(&job)==-EINVAL && !memcmp(before,pixels,sizeof(pixels))); /* B2 */
    wire.r[2].reserved[0]=0; wire.r[2].opcode=7; wire.r[2].foreground=0; wire.r[2].src=1; /* E1 */
    CHECK(display_batch_validate(&job)==-EINVAL);
    wire.r[2].opcode=8; wire.r[2].sw=4; wire.r[2].sh=1; CHECK(display_batch_validate(&job)==-EINVAL);
    /* B1: a complete prefix, then a clean COPY refusal with no CPU source. */
    setup(); activate(); make_job(3);
    wire.h.target_count=2; wire.h.payload_offset+=32; wire.h.payload_bytes-=32; job.header=wire.h;
    struct ciuki_batch_target extra={.kind=2,.handle=1,.generation=42};
    memcpy((uint8_t *)&wire+wire.h.targets_offset+32,&extra,32);
    CHECK(!fbdev_batch_video_target(42,&job.targets[1]));
    wire.r[1].opcode=6; wire.r[1].foreground=0;
    wire.r[2]=(struct ciuki_batch_record){.opcode=2,.record_bytes=128,.dst=1,.src=2,.dw=4,.dh=1};
    error_at=3; cmd_error=-EOPNOTSUPP; CHECK(!display_batch_validate(&job));
    CHECK(fbdev_batch_execute(&job)==-EAGAIN && job.reply.completed==2 && job.reply.first_uncompleted==2 && job.reply.state==3 && !(job.reply.flags&1));
    /* B3: overlapping COPY fails after FILL, with a suffix never submitted. */
    setup(); activate(); make_job(3); cmd_error=-EIO; error_at=2; stop_fail=true;
    wire.r[1]=(struct ciuki_batch_record){.opcode=2,.record_bytes=128,.dst=1,.src=1,.dx=1,.dw=4,.dh=1};
    CHECK(!display_batch_validate(&job)); CHECK(fbdev_batch_execute(&job)==-EIO);
    CHECK(job.reply.completed==1 && job.reply.state==4 && (job.reply.flags&1) && !(job.reply.flags&2));
    CHECK(drawing==PRESENT_BUSY && accel_inflight && !fbdev_console_begin() && accepted==2);
    CHECK(!fbdev_batch_recover(&job) && !locks && batch_owner==&job);
    unsafe=false; stop_fail=false; sliced=true;
    CHECK(fbdev_batch_recover(&job) && job.reply.state==3 && (job.reply.flags&2));
    CHECK(!drawing && !accel_inflight && accepted==2); /* B3/B6 */
    setup(); activate(); make_job(2); sliced=true;
    CHECK(!display_batch_validate(&job)); CHECK(fbdev_batch_execute(&job)==-ECANCELED);
    CHECK(job.reply.completed==1 && job.reply.state==3 && native_calls==1 && yields>=1); /* B5 */
    setup(); make_job(2); acceleration=NULL; wire.r[0].dw=wire.r[0].dh=0; wire.r[1]=(struct ciuki_batch_record){.opcode=10,.record_bytes=128};
    CHECK(!display_batch_validate(&job)); CHECK(!fbdev_batch_execute(&job) && job.reply.completed==2 && job.reply.state==2); /* B8 */
    setup(); activate(); make_job(1); wire.r[0].dw=0;
    CHECK(!display_batch_validate(&job) && job.noop[0]);
    CHECK(!fbdev_batch_execute(&job) && job.reply.completed==1 && !native_calls);
    bool ready,used; const char *reason;
    CHECK(display_driver_summary(&ready,&used,&reason) && !used); /* no native evidence from an empty record */
    wire.h.record_count=65; CHECK(display_batch_header(&wire,wire.h.total_bytes,&job.header)==-E2BIG);
    CHECK(display_batch_header(&wire,1048584,&job.header)==-E2BIG); /* B7 */
    setup(); make_job(1); wire.r[0].clip_count=17; CHECK(display_batch_validate(&job)==-E2BIG);
    wire.r[0].clip_count=0; wire.r[0].opcode=65535; CHECK(display_batch_validate(&job)==-EOPNOTSUPP);
    printf("batch: all native opcodes, clean punt, complete preflight, no replay, quarantine/recovery ownership, sliced mode race, limits and noops PASS\n");
}
int main(void)
{
    goldens(16); goldens(24); goldens(32); batch_tests();
    printf("display commands: %u literal pixel checks, %u failures\n",vectors,failures);
    return failures ? 1 : 0;
}
