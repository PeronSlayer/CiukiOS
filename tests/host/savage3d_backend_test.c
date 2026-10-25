/* Packet interpreter independent of the producer and scalar renderer. Uses
 * the unchanged 2D fixture's production owner, status/event and fbdev locks.
 * Host floating point decodes IEEE fixtures; kernel objects remain FP-free.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "savage2d_fixture.inc"
#include <math.h>
#include "../../src/kernel/drivers/video/s3savage/savage3d.h"
static struct savage3d_device gpu3d;
static struct render3d_service scratch3d;
static uint8_t ram3d[1024*1024];
static uint32_t batch3d[32], count3d, need3d;
static unsigned draw3d, copy3d, writes3d, memory3d, corruption3d, allocations3d;
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
#include "savage3d_sampling.inc"
static void raster3d(void)
{
    CHECK(batch3d[0]==0x800300c8u || batch3d[0]==0x80030008u);
    unsigned stride=batch3d[0]==0x80030008u ? 9 : 7;
    uint32_t zctrl=m.reg[0x485d4/4],local=m.reg[0x48584/4];
    bool depth_enabled=zctrl&0x20u,depth_write=local&0x08000000u;
    unsigned comparison=zctrl&7u;
    CHECK((zctrl&~0x27u)==0x80000000u); /* integer D16, W-to-Z */
    CHECK(m.reg[0x48578/4]==0); /* not the actual local-control address */
    CHECK(!(local&~0xde000077u) && (local&0xc4000000u)==0xc4000000u);
    CHECK(!depth_write || depth_enabled);
    CHECK(m.reg[0x485e8/4]==0x00001818u && m.reg[0x485ec/4]==0x4f000000u);
    uint32_t dc=m.reg[0x485dc/4],zc=m.reg[0x485d8/4],td=m.reg[0x485a8/4];
    bool textured=td&0x02000000u,second=td&0x04000000u;
    CHECK(second==(stride==9));
    CHECK(!!(td&0x01000000u)==second); /* required texture blending loop */
    uint32_t cb=((dc>>8)&0x3fff)*2048,zb=(zc&0x3fff)*2048;
    unsigned cp=(dc&127)*128,zp=((zc>>25)&63)*128;
    uint32_t scissor=m.reg[0x485e4/4];
    unsigned width=(scissor&0x7ff)+1,height=((scissor>>12)&0xfff)+1;
    CHECK(width<=64 && height<=64 && cp==((width*4+127)&~127u));
    CHECK(!depth_enabled || zp==((width*2+127)&~127u));
    long double x[3],y[3],z[3],u[3],v[3],u1[3],v1[3],rw[3]; uint32_t colors[3];
    for (unsigned i=0;i<3;i++) {
        const uint32_t *p=batch3d+1+i*stride;
        x[i]=ieee3d(p[0])+0.5L; y[i]=ieee3d(p[1])+0.375L;
        z[i]=ieee3d(p[2]); rw[i]=ieee3d(p[3]); colors[i]=p[4]; u[i]=ieee3d(p[5]); v[i]=ieee3d(p[6]);
        u1[i]=second ? ieee3d(p[7]) : 0; v1[i]=second ? ieee3d(p[8]) : 0;
    }
    long double area=(x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]);
    if (!area) { draw3d++; return; }
    if (area<0) {
        /* Culling is disabled. Normalize winding together with attributes. */
        long double *attributes[]={x,y,z,u,v,rw,u1,v1};
        for (unsigned i=0;i<8;i++) {
            long double tmp=attributes[i][1]; attributes[i][1]=attributes[i][2]; attributes[i][2]=tmp;
        }
        uint32_t tmp=colors[1]; colors[1]=colors[2]; colors[2]=tmp; area=-area;
    }
    bool inclusive[3];
    for (unsigned i=0;i<3;i++) {
        unsigned from=(i+1)%3,to=(i+2)%3;
        inclusive[i]=y[to]<y[from] || (y[to]==y[from] && x[to]>x[from]);
    }
    for (unsigned yy=0;yy<height;yy++) for (unsigned xx=0;xx<width;xx++) {
        long double px=xx+0.5L,py=yy+0.5L;
        /* Keep exact edge numerators until the last division. Normalizing
         * each weight first can move an exact nearest-sampling boundary. */
        long double a=(x[1]-px)*(y[2]-py)-(y[1]-py)*(x[2]-px);
        long double b=(x[2]-px)*(y[0]-py)-(y[2]-py)*(x[0]-px);
        long double c=(x[0]-px)*(y[1]-py)-(y[0]-py)*(x[1]-px);
        if (a<0 || b<0 || c<0 || (!a && !inclusive[0]) || (!b && !inclusive[1]) || (!c && !inclusive[2])) continue;
        unsigned depth=(unsigned)((a*z[0]+b*z[1]+c*z[2])*65535/area+0.5L);
        uint32_t da=depth_enabled ? destination3d(zb,zp,2,xx,yy) : 0;
        if (!dropping3d && depth_enabled && !compare3d(depth,load3d(da,2),comparison)) continue;
        long double rhw=a*rw[0]+b*rw[1]+c*rw[2];
        uint32_t color=0;
        for (unsigned shift=0;shift<32;shift+=8) {
            long double diffuse=(a*((colors[0]>>shift)&255)+b*((colors[1]>>shift)&255)+c*((colors[2]>>shift)&255))/area;
            color|=(uint32_t)(diffuse+.5L)<<shift;
        }
        uint32_t diffuse=color;
        for (unsigned stage=0;stage<2;stage++) {
            if (!(stage ? second : textured)) continue;
            long double *us=stage ? u1 : u,*vs=stage ? v1 : v;
            long double uu=(a*us[0]*rw[0]+b*us[1]*rw[1]+c*us[2]*rw[2])/rhw;
            long double vv=(a*vs[0]*rw[0]+b*vs[1]*rw[1]+c*vs[2]*rw[2])/rhw;
            long double rho=0;
            for (unsigned axis=0;axis<2;axis++) {
                long double aa=a+(axis ? x[2]-x[1] : y[1]-y[2]);
                long double bb=b+(axis ? x[0]-x[2] : y[2]-y[0]);
                long double cc=area-aa-bb,den=aa*rw[0]+bb*rw[1]+cc*rw[2];
                long double un=(aa*us[0]*rw[0]+bb*us[1]*rw[1]+cc*us[2]*rw[2])/den;
                long double vn=(aa*vs[0]*rw[0]+bb*vs[1]*rw[1]+cc*vs[2]*rw[2])/den;
                unsigned desc=td>>(stage*12),tw=1u<<(desc&15),th=1u<<(desc>>4&15);
                long double du=fabsl(un-uu)*tw,dv=fabsl(vn-vv)*th;
                if (du>rho) rho=du;
                if (dv>rho) rho=dv;
            }
            color=combine3d(diffuse,color,sample3d(stage,uu,vv,rho),stage);
        }
        uint32_t dc1=m.reg[0x485e4/4];
        if ((dc1&0x80000000u) && !compare3d(color>>24,m.reg[0x485e0/4]>>24,dc1>>28&7)) continue;
        CHECK(!(dc1&0x80000000u) || (local&0x02000000u));
        uint32_t ca=destination3d(cb,cp,4,xx,yy);
        store3d(ca,blend3d(color,load3d(ca,4),local),4);
        if (depth_write) store3d(da,depth,2);
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
        unsigned w=batch3d[7]&65535,h=batch3d[7]>>16;
        CHECK(batch3d[2]==(0x10200000u|w) && batch3d[4]==(0x11200000u|((w+31)&~31u)));
        m.reg[0x8178/4]=batch3d[1]; m.reg[0x817c/4]=batch3d[2];
        m.reg[0x8170/4]=batch3d[3]; m.reg[0x8174/4]=batch3d[4];
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
            uint32_t value=load3d(destination3d(batch3d[3],(batch3d[4]&65535)*4,4,x,y),4);
            store3d(batch3d[1]+(y*w+x)*4,value^(corruption3d==2 ? 1u : 0u),4);
        }
        copy3d++;
    } else if ((cmd==0x800300c8 || cmd==0x80030008)) raster3d();
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
            value==0x80030008 ? 28 : value==0x800300c8 ? 22 : value==0x4bcc8c00 ? 6 : value==0x4bcc14c0 ? 8 : 0;
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
static void *allocate3d(void *ctx,size_t bytes)
{ (void)ctx; void *p=malloc(bytes); if (p) allocations3d++; return p; }
static void free3d(void *ctx,void *p) { (void)ctx; CHECK(p && allocations3d); allocations3d--; free(p); }
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
    CHECK(caps.hardware_features==R3D_ALL_FEATURES && caps.hardware_max_size==64);
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
    CHECK(!allocations3d && !scratch3d.ram_bytes);
    CHECK(render3d_submit(&scratch3d,7,c,&q,sizeof(q),&fence)<0 && writes3d==before);
    bound=0;
}
static void variants3d(const struct render3d_host *host)
{
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    render3d_init(&scratch3d,host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    const unsigned sizes[][2]={{1,1},{7,19},{31,17},{32,16},{33,31},{63,47},{64,64}};
    const unsigned textures[][2]={{32,16},{32,32},{32,64},{64,16},{64,32},{64,64}};
    unsigned cases=0,texture_sizes=0;
    for (unsigned size=0;size<7;size++) for (unsigned flags=1;flags<=R3D_FEATURES;flags++)
    for (unsigned draw=0;draw<2;draw++) {
        if (!draw && (flags&R3D_TEXTURED)) continue;
        unsigned w=sizes[size][0],h=sizes[size][1],t=(size+flags/2)%6;
        unsigned tw=textures[t][0],th=textures[t][1];
        uint32_t texture[64*64];
        for (unsigned y=0;y<th;y++) for (unsigned x=0;x<tw;x++)
            texture[y*tw+x]=0xff000000u|((x*3+1)<<16)|((y*3+2)<<8)|((x+y)*2+3);
        uint32_t c,handles[4]; CHECK(!render3d_create_context(&scratch3d,7,R3D_HARDWARE_REQUIRED,&c));
        const enum render3d_kind kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
        struct render3d_resource *r[R3D_REFS]={0};
        for (unsigned i=0;i<4;i++) {
            CHECK(!render3d_create_resource(&scratch3d,7,c,kinds[i],i==2 ? tw : w,i==2 ? th : h,
                i==2 ? texture : 0,i==2 ? tw*th*4 : 0,&handles[i]));
            r[i]=&scratch3d.contexts[0].resources[i];
        }
        for (unsigned i=0;i<w*h;i++) {
            ((uint32_t *)r[0]->shadow)[i]=0xff203040;
            ((uint16_t *)r[1]->shadow)[i]=32768;
            ((uint32_t *)r[3]->shadow)[i]=0xff010101;
        }
        unsigned depth=flags&R3D_CLEAR_COLOR ? 49152 : 16384;
        struct render3d_request q={.flags=flags,.target=handles[0],.count=draw ? 3 : 0,
            .depth=flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE) ? handles[1] : 0,
            .texture=flags&R3D_TEXTURED ? handles[2] : 0,.copy_target=flags&R3D_COPY ? handles[3] : 0,
            .clear_color=0xff102030,.clear_depth=40000};
        bool perspective=flags&R3D_COPY;
        if (draw) {
            q.vertices[0]=(struct render3d_vertex){0,0,depth,65536,0xff804020,0,0};
            q.vertices[1]=(struct render3d_vertex){w<<16,0,depth,perspective ? 4*65536 : 65536,0xff804020,65536,0};
            q.vertices[2]=(struct render3d_vertex){0,h<<16,depth,65536,0xff804020,0,65536};
            if (flags&R3D_DEPTH_WRITE) {
                struct render3d_vertex tmp=q.vertices[1]; q.vertices[1]=q.vertices[2]; q.vertices[2]=tmp;
            }
        }
        unsigned old_draws=draw3d,old_copies=copy3d;
        struct render3d_fence fence;
        CHECK(!render3d_submit(&scratch3d,7,c,&q,sizeof(q),&fence));
        CHECK(render3d_step(&scratch3d)==1 && !render3d_wait(&scratch3d,7,c,fence,0));
        CHECK(draw3d==old_draws+draw && copy3d==old_copies+!!(flags&R3D_COPY));
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
            uint32_t expected=flags&R3D_CLEAR_COLOR ? q.clear_color : 0xff203040;
            unsigned expected_z=flags&R3D_CLEAR_DEPTH ? q.clear_depth : 32768;
            /* Analytic right triangle; strict hypotenuse, inclusive top/left.
             * Exact rational UV avoids sharing the packet interpreter's FP
             * barycentric implementation or the production scalar renderer. */
            if (draw && (2*x+1)*h+(2*y+1)*w<2*w*h && (!(flags&R3D_DEPTH_TEST) || depth<expected_z)) {
                expected=0xff804020;
                if (flags&R3D_TEXTURED) {
                    unsigned denom=2*w+(perspective ? 3*(2*x+1) : 0);
                    unsigned tx=tw*(2*x+1)*(perspective ? 4 : 1)/denom;
                    unsigned ty=th*(2*y+1)*w/(h*denom);
                    uint32_t texel=texture[ty*tw+tx]; expected=0;
                    for (unsigned channel=0;channel<4;channel++) {
                        unsigned shift=channel*8;
                        expected|=((((0xff804020u>>shift)&255)*((texel>>shift)&255)+127)/255)<<shift;
                    }
                    texture_sizes|=1u<<t;
                }
                if (flags&R3D_DEPTH_WRITE) expected_z=depth;
            }
            if (((uint32_t *)r[0]->shadow)[y*w+x]!=expected)
                fprintf(stderr,"variant %ux%u tex=%ux%u flags=%x draw=%u pixel=%u,%u color=%08x expected=%08x\n",
                    w,h,tw,th,flags,draw,x,y,((uint32_t *)r[0]->shadow)[y*w+x],expected);
            CHECK(((uint32_t *)r[0]->shadow)[y*w+x]==expected);
            CHECK(((uint16_t *)r[1]->shadow)[y*w+x]==expected_z);
            CHECK(((uint32_t *)r[3]->shadow)[y*w+x]==(flags&R3D_COPY ? expected : 0xff010101u));
            if (failures) exit(1);
        }
        CHECK(!render3d_destroy_context(&scratch3d,7,c) && !allocations3d && !scratch3d.ram_bytes);
        CHECK(!locks && !drawing && !accel_inflight && !count3d);
        CHECK(!m.reg[0x48c40/4] && !m.reg[0x48584/4] && !m.reg[0x485d4/4]);
        cases++;
    }
    CHECK(cases==658 && texture_sizes==63);
    CHECK(!render3d_close(&scratch3d) && !allocations3d && !scratch3d.ram_bytes);
    printf("Savage variants: %u cases, 7 target sizes, 6 texture sizes, all 63 flag combinations, empty lists, perspective, depth masks and conversion PASS\n",cases);
}
#include "savage3d_fixed_function.inc"
static void backend3d(unsigned fault,bool variants)
{
    CHECK(!allocations3d);
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
        r->hardware=true; r->offset=i==2 ? gpu3d.heap.first+gpu3d.heap.bytes-SAVAGE3D_TEXTURE_MIN : at;
        CHECK(!savage3d_backend()->layout(&gpu3d,r)); if (i!=2) at+=r->reserved;
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
        struct render3d_resource *r[R3D_REFS]={0}; for (unsigned i=0;i<4;i++) r[i]=&scratch3d.contexts[0].resources[i];
        struct render3d_request q={.flags=R3D_CLEAR_COLOR,.clear_color=0,.count=0};
        unsigned before=writes3d; r[0]->offset-=4096;
        CHECK(savage3d_backend()->validate(&gpu3d,&q,r)==-EINVAL && writes3d==before);
        r[0]->offset+=4096;
        if (variants) { variants3d(&host); fixed_function3d(&host); texture_security3d(&host); texture_lru3d(&host); mip_addresses3d(); }
        native_consumer3d(&host,texture);
        test_engine.io.write32=model_write;
        struct fb_rect pixel={0,0,1,1}; CHECK(!test_engine.ops.fill_rect(&test_engine,&pixel,0xabcdef));
    } else if (fault==5) CHECK(!writes3d && !memory3d && !gpu3d.lost);
    else CHECK(gpu3d.lost && test_engine.quarantined && !test_engine.active);
    /* Engine destroyed before fixture frees (models reboot, no recovery API). */
    CHECK(!render3d_close(&scratch3d) && !allocations3d && !scratch3d.ram_bytes); finish();
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"variants")) backend3d(0,true);
    else for (unsigned i=0;i<6;i++) backend3d(i,i==0);
    printf("Savage production packets/owner: positive textured Gouraud D16 probe, conversion, guards, restoration, rejection/fault containment: %s\n",failures ? "FAIL" : "PASS");
    fflush(stdout); return failures ? 1 : 0;
}
