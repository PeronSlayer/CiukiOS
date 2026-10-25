/* Packet interpreter independent of the producer and scalar renderer. Uses
 * the unchanged 2D fixture's production owner, status/event and fbdev locks.
 * Host floating point decodes IEEE fixtures; kernel objects remain FP-free.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "savage2d_fixture.inc"
#include "../../src/kernel/drivers/video/s3savage/savage3d.h"
static struct savage3d_device gpu3d;
static struct render3d_service scratch3d;
static uint8_t ram3d[128*1024];
static uint32_t batch3d[32], count3d, need3d;
static unsigned draw3d, copy3d, writes3d, memory3d, corruption3d;
static bool dropping3d, bad_restore3d;
static uint32_t physical3d(uint32_t address,unsigned bytes)
{
    CHECK(address>=gpu3d.heap.first && (uint64_t)address+bytes<=gpu3d.heap.first+gpu3d.heap.bytes);
    return address-gpu3d.heap.first;
}
/* Independent destination tile model: 128 byte rows x 16 rows / tile.
 * A tiled aperture is an alias, and its 4/8 KiB pitch is not physical pitch. */
static uint32_t destination3d(uint32_t base,unsigned pitch,unsigned bytes,unsigned x,unsigned y)
{
    unsigned byte_x=x*bytes, tiles=pitch/128;
    return base+(y/16*tiles+byte_x/128)*2048+(y%16)*128+byte_x%128;
}
static uint32_t load3d(uint32_t address,unsigned bytes)
{
    uint32_t value=0; memcpy(&value,ram3d+physical3d(address,bytes),bytes); return value;
}
static void store3d(uint32_t address,uint32_t value,unsigned bytes)
{ memcpy(ram3d+physical3d(address,bytes),&value,bytes); }
static long double ieee3d(uint32_t word)
{ float v; memcpy(&v,&word,4); return v; }
static void raster3d(void)
{
    CHECK(batch3d[0]==0x800300c8u);
    CHECK(m.reg[0x485d4/4]==0x80000024u); /* integer D16, reversed GREATER */
    CHECK(m.reg[0x48578/4]==0); /* not the actual local-control address */
    CHECK(m.reg[0x48584/4]==0xcc000010u);
    CHECK(m.reg[0x4858c/4]==0x0005f040u);
    CHECK(m.reg[0x485e8/4]==0x00001818u && m.reg[0x485ec/4]==0x4f000000u);
    CHECK(m.reg[0x4859c/4]==0x00110011u && m.reg[0x485a0/4]==0x00870407u);
    uint32_t dc=m.reg[0x485dc/4],zc=m.reg[0x485d8/4],td=m.reg[0x485a8/4];
    CHECK((td&0x02000f00u)==0x02000300u);
    uint32_t cb=((dc>>8)&0x3fff)*2048,zb=(zc&0x3fff)*2048,tb=m.reg[0x48594/4]&~7u;
    unsigned cp=(dc&127)*128,zp=((zc>>25)&63)*128,tw=1u<<(td&15),th=1u<<((td>>4)&15);
    CHECK(cp==256 && zp==128 && tw==32 && th==16);
    long double x[3],y[3],z[3],u[3],v[3],rw[3]; uint32_t colors[3];
    for (unsigned i=0;i<3;i++) {
        const uint32_t *p=batch3d+1+i*7;
        x[i]=ieee3d(p[0])+0.5L; y[i]=ieee3d(p[1])+0.375L;
        z[i]=ieee3d(p[2]); rw[i]=ieee3d(p[3]); colors[i]=p[4]; u[i]=ieee3d(p[5]); v[i]=ieee3d(p[6]);
    }
    long double area=(x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]);
    CHECK(area>0);
    for (unsigned yy=0;yy<64;yy++) for (unsigned xx=0;xx<64;xx++) {
        long double px=xx+0.5L,py=yy+0.5L;
        long double a=((x[1]-px)*(y[2]-py)-(y[1]-py)*(x[2]-px))/area;
        long double b=((x[2]-px)*(y[0]-py)-(y[2]-py)*(x[0]-px))/area,c=1-a-b;
        if (a<0 || b<0 || c<0 || a==0) continue;
        unsigned depth=(unsigned)((a*z[0]+b*z[1]+c*z[2])*65535+0.5L);
        uint32_t da=destination3d(zb,zp,2,xx,yy);
        if (!dropping3d && depth<=load3d(da,2)) continue;
        long double rhw=a*rw[0]+b*rw[1]+c*rw[2];
        unsigned tx=(unsigned)((a*u[0]*rw[0]+b*u[1]*rw[1]+c*u[2]*rw[2])/rhw*tw);
        unsigned ty=(unsigned)((a*v[0]*rw[0]+b*v[1]*rw[1]+c*v[2]*rw[2])/rhw*th);
        if (tx>=tw) tx=tw-1;
        if (ty>=th) ty=th-1;
        /* Pro texture tile: 8x2 texels per 64-byte subtile, four columns. */
        uint32_t to=(ty/16*(tw/32)+tx/32)*2048+((ty/2%8)*4+(tx/8%4))*64+(ty%2*8+tx%8)*4;
        uint32_t texel=load3d(tb+to,4),color=0;
        for (unsigned channel=0;channel<4;channel++) {
            unsigned shift=channel*8;
            long double diffuse=a*((colors[0]>>shift)&255)+b*((colors[1]>>shift)&255)+c*((colors[2]>>shift)&255);
            unsigned value=(unsigned)(diffuse*((texel>>shift)&255)/255+0.5L);
            color|=value<<shift;
        }
        store3d(destination3d(cb,cp,4,xx,yy),color,4); store3d(da,depth,2);
    }
    draw3d++;
}
static void interpret3d(void)
{
    uint32_t cmd=batch3d[0];
    if ((cmd&0xff000000)==0x96000000) {
        unsigned n=(cmd>>16)&255,reg=cmd&65535;
        CHECK(n+1==need3d);
        for (unsigned i=0;i<n;i++) {
            uint32_t addr=reg+i>=0xe2 ? 0x8170+(reg+i-0xe2)*4 : 0x4850c+(reg+i)*4;
            CHECK(addr!=0x8168 && addr!=0x816c); /* retained GBD */
            if (!(bad_restore3d && reg==0x1e && draw3d)) m.reg[addr/4]=batch3d[i+1];
        }
    } else if (cmd==0x4bcc8c00) {
        CHECK(need3d==6);
        uint32_t bd=batch3d[2]; unsigned bytes=(bd>>16&255)/8,w=batch3d[5]&65535,h=batch3d[5]>>16;
        CHECK((bd&0x13000000u)==0x11000000u && !batch3d[4]);
        m.reg[0x8170/4]=batch3d[1]; m.reg[0x8174/4]=bd;
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
            store3d(destination3d(batch3d[1],(bd&65535)*bytes,bytes,x,y),batch3d[3],bytes);
    } else if (cmd==0x4bcc14c0) {
        CHECK(need3d==8 && !batch3d[5] && !batch3d[6]);
        CHECK(batch3d[2]==0x10200040 && batch3d[4]==0x11200040);
        unsigned w=batch3d[7]&65535,h=batch3d[7]>>16;
        m.reg[0x8178/4]=batch3d[1]; m.reg[0x817c/4]=batch3d[2];
        m.reg[0x8170/4]=batch3d[3]; m.reg[0x8174/4]=batch3d[4];
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
            uint32_t value=load3d(destination3d(batch3d[3],256,4,x,y),4);
            store3d(batch3d[1]+(y*w+x)*4,value^(corruption3d==2 ? 1u : 0u),4);
        }
        copy3d++;
    } else if (cmd==0x800300c8) raster3d();
    else CHECK(false);
    if (corruption3d==1 && draw3d) ram3d[0]=0;
}
static void write_engine3d(void *ctx,uint32_t off,uint32_t value)
{
    CHECK(locks==1);
    if (off<SAVAGE_BCI || off>=SAVAGE_BCI+128) {
        CHECK(off==0x48c40 || off==0x850c || test_engine.quarantined);
        model_write(ctx,off,value); return;
    }
    if (!count3d) {
        need3d=(value&0xff000000)==0x96000000 ? ((value>>16)&255)+1 :
            value==0x800300c8 ? 22 : value==0x4bcc8c00 ? 6 : value==0x4bcc14c0 ? 8 : 0;
        CHECK(need3d && need3d<=30);
    }
    CHECK(off==SAVAGE_BCI+count3d*4 && count3d<32);
    batch3d[count3d++]=value; writes3d++;
    if (count3d==need3d+2) {
        CHECK(batch3d[need3d]==0xc0030000u && (value&0xffff0000u)==0x98000000u && (value&65535));
        interpret3d(); m.reg[SAVAGE_EVENT_STATUS/4]=value&65535; count3d=0;
    }
}
static uint32_t memory_read3d(void *ctx,bool alias,uint32_t offset,unsigned bytes)
{
    (void)ctx; CHECK(locks==1); memory3d++;
    if (alias) {
        uint32_t desc=m.reg[0x48c40/4]; unsigned bpp=desc>>30,physical_pitch=(desc>>20&63)*128;
        CHECK(bpp==2 || bpp==3);
        unsigned pitch=bpp==2 ? 4096 : 8192;
        offset=destination3d((desc&0xfffffu)<<6,physical_pitch,bytes,(offset%pitch)/bytes,offset/pitch);
    }
    return load3d(offset,bytes);
}
static void memory_write3d(void *ctx,bool alias,uint32_t offset,uint32_t value,unsigned bytes)
{
    (void)ctx; CHECK(locks==1); memory3d++;
    if (alias) {
        uint32_t desc=m.reg[0x48c40/4]; unsigned bpp=desc>>30,pitch=bpp==2 ? 4096 : 8192;
        offset=destination3d((desc&0xfffffu)<<6,(desc>>20&63)*128,bytes,(offset%pitch)/bytes,offset/pitch);
    }
    store3d(offset,value,bytes);
}
static int lock3d(void *ctx,uint32_t generation) { (void)ctx; return fbdev_render3d_enter(generation); }
static void unlock3d(void *ctx) { (void)ctx; fbdev_render3d_leave(); }
static void *allocate3d(void *ctx,size_t bytes) { (void)ctx; return malloc(bytes); }
static void free3d(void *ctx,void *p) { (void)ctx; free(p); }
static uint64_t clock3d(void *ctx) { (void)ctx; return m.us; }
/* The owner attachment is a hardware-boundary fixture. The native bridge,
 * registry nomination, service and actual executing backend are production. */
int savage3d_owner_attach(struct render3d_service *s,const struct render3d_host *h,uint32_t generation)
{
    if (!gpu3d.qualified || generation!=gpu3d.generation) return -ENODEV;
    if (gpu3d.consumer) return -EBUSY;
    render3d_init(s,h,generation,savage3d_backend(),&gpu3d,gpu3d.heap);
    gpu3d.consumer=s; return 0;
}
static const struct render3d_family native_family3d={.status=R3D_UNQUALIFIED,.backend=savage3d_backend};
static const struct display_driver native_driver3d={.render3d=&native_family3d};
static void native_consumer3d(const struct render3d_host *host,const uint32_t *texture)
{
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    bound=&native_driver3d;
    CHECK(!render3d_native_init(&scratch3d,host));
    struct render3d_caps caps; render3d_query_caps(&scratch3d,&caps);
    CHECK(caps.hardware_features==R3D_FEATURES && caps.hardware_max_size==64);
    uint32_t c,handles[4]; CHECK(!render3d_create_context(&scratch3d,7,R3D_HARDWARE_REQUIRED,&c));
    enum render3d_kind kind[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
    for (unsigned i=0;i<4;i++) CHECK(!render3d_create_resource(&scratch3d,7,c,kind[i],i==2 ? 32 : 64,i==2 ? 16 : 64,
        i==2 ? texture : 0,i==2 ? 2048 : 0,&handles[i]));
    struct render3d_request q={.flags=R3D_FEATURES,.count=3,.clear_depth=65535,.target=handles[0],
        .depth=handles[1],.texture=handles[2],.copy_target=handles[3]};
    q.vertices[0]=(struct render3d_vertex){8<<16,8<<16,16384,65536,0xffffffffu,0,0};
    q.vertices[1]=(struct render3d_vertex){56<<16,8<<16,16384,65536,0xffffffffu,65536,0};
    q.vertices[2]=(struct render3d_vertex){8<<16,56<<16,16384,65536,0xffffffffu,0,65536};
    struct render3d_fence fence;
    unsigned before=writes3d;
    CHECK(render3d_submit(&scratch3d,8,c,&q,sizeof(q),&fence)==-ENOENT && writes3d==before);
    CHECK(!render3d_submit(&scratch3d,7,c,&q,sizeof(q),&fence));
    CHECK(render3d_step(&scratch3d)==1 && !render3d_wait(&scratch3d,7,c,fence,0));
    uint32_t pixels[64*64]; CHECK(!render3d_readback(&scratch3d,7,c,handles[3],pixels,sizeof(pixels)));
    CHECK((pixels[16*64+16]&0xffffffu)==0x4080c0u && (pixels[12*64+48]&0xffffffu)==0xc04080u);
    CHECK(!render3d_submit(&scratch3d,7,c,&q,sizeof(q),&fence));
    before=writes3d; CHECK(!render3d_close(&scratch3d) && !gpu3d.consumer && writes3d==before);
    CHECK(render3d_submit(&scratch3d,7,c,&q,sizeof(q),&fence)<0 && writes3d==before);
    bound=0;
}
static void backend3d(unsigned fault)
{
    prepare(32); m.reg[0x48578/4]=0;
    uint32_t old_gbd[2]={m.reg[0x8168/4],m.reg[0x816c/4]};
    memset(ram3d,0x7a,sizeof(ram3d)); draw3d=copy3d=writes3d=memory3d=count3d=0;
    corruption3d=fault==1 || fault==2 ? fault : 0; dropping3d=fault==3; bad_restore3d=fault==4;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d}};
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    render3d_init(&scratch3d,&host,42,0,0,(struct render3d_extent){0});
    uint32_t c,h; CHECK(!render3d_create_context(&scratch3d,1,R3D_SOFTWARE,&c));
    uint32_t texture[512]; for (unsigned i=0;i<512;i++) texture[i]=(i%32)<16 ? 0xff4080c0 : 0xffc04080;
    enum render3d_kind kind[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
    uint32_t at=gpu3d.heap.first;
    for (unsigned i=0;i<4;i++) {
        CHECK(!render3d_create_resource(&scratch3d,1,c,kind[i],i==2 ? 32 : 64,i==2 ? 16 : 64,
            i==2 ? texture : 0,i==2 ? sizeof(texture) : 0,&h));
        struct render3d_resource *r=&scratch3d.contexts[0].resources[i];
        r->hardware=true; r->offset=at; CHECK(!savage3d_backend()->layout(&gpu3d,r)); at+=r->reserved;
    }
    gpu3d.consumer=&scratch3d; test_engine.io.write32=write_engine3d;
    CHECK(!fbdev_render3d_prepare(42)); CHECK(fbdev_change_begin()==-EBUSY);
    if (fault==5) scratch3d.contexts[0].resources[1].offset=gpu3d.heap.first-4096;
    int e=savage3d_probe(&gpu3d);
    fbdev_render3d_prepared();
    CHECK((e==0)==(fault==0)); CHECK(!locks && !drawing && !accel_inflight);
    CHECK(m.reg[0x8168/4]==old_gbd[0] && m.reg[0x816c/4]==old_gbd[1]);
    if (!fault) {
        CHECK(gpu3d.qualified && test_engine.render3d_qualified && draw3d==3 && copy3d==3);
        CHECK(!m.reg[0x48c40/4] && !m.reg[0x850c/4] && !m.reg[0x48584/4]);
        struct render3d_resource *r[4]; for (unsigned i=0;i<4;i++) r[i]=&scratch3d.contexts[0].resources[i];
        struct render3d_request q={.flags=R3D_CLEAR_COLOR,.clear_color=0,.count=0};
        unsigned before=writes3d; r[0]->offset-=4096;
        CHECK(savage3d_backend()->validate(&gpu3d,&q,r)==-EINVAL && writes3d==before);
        r[0]->offset+=4096;
        native_consumer3d(&host,texture);
        test_engine.io.write32=model_write;
        struct fb_rect pixel={0,0,1,1}; CHECK(!test_engine.ops.fill_rect(&test_engine,&pixel,0xabcdef));
    } else if (fault==5) CHECK(!writes3d && !memory3d && !gpu3d.lost);
    else CHECK(gpu3d.lost && test_engine.quarantined && !test_engine.active);
    /* Engine destroyed before fixture frees (models reboot, no recovery API). */
    CHECK(!render3d_close(&scratch3d)); finish();
}
int main(void)
{
    for (unsigned i=0;i<6;i++) backend3d(i);
    printf("Savage production packets/owner: positive textured Gouraud D16 probe, conversion, guards, restoration, rejection/fault containment: %s\n",failures ? "FAIL" : "PASS");
    fflush(stdout); return failures ? 1 : 0;
}
