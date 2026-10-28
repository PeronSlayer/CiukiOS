/* Scalar differential, malformed requests and guarded boot qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "radeon3d_model.inc"
static const struct render3d_host host3d={0,allocate3d,free3d,clock3d,0};
static void setup3d(bool r200,unsigned w,unsigned h)
{
    prepare(32);test_engine.group_id=r200 ? RADEON_R200 : RADEON_R100;
    gpu3d=(struct radeon3d_device){.engine=&test_engine,.heap={0x400000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.r200=r200,
        .qualified=true,.fullsize_testing=true,.mode_width=w,.mode_height=h,.texture_bytes=512*1024};
    fullsize_resources3d(&scratch3d,&host3d,w,h,true);
    fullsize_resources3d(&reference3d,&host3d,w,h,false);
    gpu3d.consumer=&scratch3d;
}
static void teardown3d(void)
{
    CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d));gpu3d.consumer=0;finish();
}
static void compare_scene3d(const struct render3d_request *q)
{
    struct render3d_resource *hw[5],*ref[5];
    for(unsigned i=0;i<5;i++) { hw[i]=&scratch3d.contexts[0].resources[i];ref[i]=&reference3d.contexts[0].resources[i]; }
    if(!(q->flags&R3D_TEXTURED)) hw[2]=ref[2]=0;
    if(!(q->flags&R3D_TEXTURE1)) hw[4]=ref[4]=0;
    int admission=radeon3d_backend()->validate(&gpu3d,q,hw);
    if(admission) fprintf(stderr,"admission=%d format=%u combine=%u flags=%u offsets=%x/%x sizes=%x/%x\n",admission,hw[2] ? hw[2]->format : 0,q->state.sampler[0].combine,q->flags,hw[2] ? hw[2]->offset : 0,hw[4] ? hw[4]->offset : 0,hw[2] ? hw[2]->reserved : 0,hw[4] ? hw[4]->reserved : 0);
    CHECK(!admission);
    struct render3d_job job={0};while(!render3d_scalar_slice(q,ref,&job,&host3d)) {}
    CHECK(!radeon3d_backend()->enter(&gpu3d));
    CHECK(!radeon3d_backend()->execute(&gpu3d,q,hw));radeon3d_backend()->leave(&gpu3d);
    for(unsigned i=0;i<hw[0]->width*hw[0]->height;i++) {
        uint32_t a=((uint32_t *)hw[0]->shadow)[i],b=((uint32_t *)ref[0]->shadow)[i];
        CHECK((a==q->clear_color)==(b==q->clear_color));
        for(unsigned shift=0;shift<32;shift+=8) { int d=(int)(a>>shift&255)-(int)(b>>shift&255);if(abs(d)>2) fprintf(stderr,"pixel=%u a=%08x b=%08x depth=%u filter=%u combine=%u flat=%u blend=%u alpha=%u r200=%u\n",i,a,b,q->state.depth_func,q->state.sampler[0].filter,q->state.sampler[0].combine,q->state.flat,q->state.blend,q->state.alpha_test,gpu3d.r200);CHECK(abs(d)<=2); }
        CHECK(abs((int)((uint16_t *)hw[1]->shadow)[i]-(int)((uint16_t *)ref[1]->shadow)[i])<=1);
        CHECK(a==((uint32_t *)hw[3]->shadow)[i]);
    }
}
static void differential3d(bool r200)
{
    setup3d(r200,64,64);gpu3d.fullsize_testing=false;gpu3d.fullsize_qualified=gpu3d.perspective_qualified=true;
    static struct render3d_request q;
    unsigned cases=0;
    for(unsigned depth=0;depth<8;depth++) for(unsigned filter=0;filter<5;filter++) for(unsigned combine=0;combine<4;combine++)
    for(unsigned state=0;state<8;state++) {
        q=(struct render3d_request){.flags=RADEON3D_FEATURES,.count=3,.clear_depth=32768,.clear_color=0x80406080,
            .state={.depth_func=depth,.flat=state&1,.blend=(state>>1)&1,.source=R3D_SRC_ALPHA,.destination=R3D_INV_SRC_ALPHA,
                .alpha_test=(state>>2)&1,.alpha_func=depth,.alpha_ref=128}};
        for(unsigned t=0;t<2;t++) q.state.sampler[t]=(struct render3d_sampler){combine,filter,state&1,state&1};
        q.flat_color[0]=0x8090b0d0;
        q.vertices[0]=(struct render3d_vertex){8<<16,8<<16,16000,65536,0x40909090,0,0};
        q.vertices[1]=(struct render3d_vertex){56<<16,8<<16,48000,65536,0xc0d0d0d0,65536,0};
        q.vertices[2]=(struct render3d_vertex){8<<16,56<<16,16000,65536,0x80808080,0,65536};
        if(state&4) { struct render3d_vertex tmp=q.vertices[1];q.vertices[1]=q.vertices[2];q.vertices[2]=tmp; }
        for(unsigned i=0;i<3;i++) { q.uv1[i][0]=q.vertices[i].v;q.uv1[i][1]=q.vertices[i].u; }
        if(state==0) q.flags&=~(R3D_TEXTURED|R3D_TEXTURE1);
        compare_scene3d(&q);cases++;
    }
    printf("R%u scalar differential: %u flat/Gouraud/depth/alpha/blend/two-stage/mip cases PASS\n",r200 ? 200 : 100,cases);
    teardown3d();
}
static struct render3d_request basic3d(void);
static void formats_blends3d(bool r200)
{
    setup3d(r200,64,64);gpu3d.fullsize_testing=false;gpu3d.fullsize_qualified=gpu3d.perspective_qualified=true;
    unsigned cases=0;
    for(unsigned fmt=0;fmt<6;fmt++) {
        for(unsigned side=0;side<2;side++) for(unsigned stage=2;stage<=4;stage+=2) {
            struct render3d_resource *t=&(side ? reference3d : scratch3d).contexts[0].resources[stage];
            t->format=fmt;t->resident=false;
            uint32_t *pixels=t->shadow;
            for(unsigned i=0;i<t->bytes/4;i++) {
                uint32_t v=0x80a0c0e0;
                if(fmt==R3D_RGB565) v=0xffa5c3e7;
                if(fmt==R3D_ARGB4444) v=0x88aaccee;
                if(fmt==R3D_L8) v=0xff808080;
                if(fmt==R3D_A8) v=0x80ffffff;
                pixels[i]=v;
            }
            if(!side) CHECK(!radeon3d_backend()->layout(&gpu3d,t));
        }
        for(unsigned combine=0;combine<4;combine++) {
            struct render3d_request q=basic3d();
            q.state.sampler[0].combine=q.state.sampler[1].combine=combine;
            if(fmt==R3D_A8 && combine==R3D_DECAL) {
                struct render3d_resource *r[5];for(unsigned i=0;i<5;i++) r[i]=&scratch3d.contexts[0].resources[i];
                CHECK(radeon3d_backend()->validate(&gpu3d,&q,r)==-EOPNOTSUPP);continue;
            }
            compare_scene3d(&q);cases++;
        }
    }
    for(unsigned sf=0;sf<10;sf++) for(unsigned df=0;df<10;df++) {
        if(sf==R3D_SRC_COLOR || sf==R3D_INV_SRC_COLOR || df==R3D_DST_COLOR || df==R3D_INV_DST_COLOR) continue;
        struct render3d_request q=basic3d();q.flags&=~(R3D_TEXTURED|R3D_TEXTURE1);
        q.state.blend=1;q.state.source=sf;q.state.destination=df;
        for(unsigned i=0;i<3;i++) q.vertices[i].color=0x8090b0d0;
        compare_scene3d(&q);cases++;
    }
    printf("R%u six texture formats/four combiners/all 64 legal blend pairs: %u cases PASS\n",r200 ? 200 : 100,cases);
    teardown3d();
}
static void full_probe3d(bool r200,unsigned w,unsigned h)
{
    printf("begin R%u fullsize %ux%u\n",r200 ? 200 : 100,w,h);
    setup3d(r200,w,h);gpu3d.fullsize_testing=false;
    int e=radeon3d_fullsize_probe(&gpu3d,&reference3d);
    if(e || !gpu3d.perspective_qualified) fprintf(stderr,"probe e=%d precision=%d phase=%u expected=%08x actual=%08x\n",e,gpu3d.precision_error,gpu3d.precision_phase,gpu3d.expected,gpu3d.actual);
    CHECK(!e && gpu3d.fullsize_qualified && gpu3d.perspective_qualified);
    printf("R%u %ux%u fullsize/23-phase precision PASS samples=%u/%u levels=%u/%u\n",r200 ? 200 : 100,w,h,gpu3d.sample_count,gpu3d.precision_samples,gpu3d.measured_filter_levels[0],gpu3d.measured_filter_levels[1]);
    teardown3d();
}
static struct render3d_request basic3d(void)
{
    struct render3d_request q={.flags=RADEON3D_FEATURES,.count=3,.clear_depth=65535,.clear_color=0x80406080};
    q.vertices[0]=(struct render3d_vertex){8<<16,8<<16,16384,65536,0xffffffffu,0,0};
    q.vertices[1]=(struct render3d_vertex){56<<16,8<<16,16384,65536,0xffffffffu,65536,0};
    q.vertices[2]=(struct render3d_vertex){8<<16,56<<16,16384,65536,0xffffffffu,0,65536};
    return q;
}
static void minimum3d(bool r200,unsigned fault)
{
    prepare(32);test_engine.group_id=r200 ? RADEON_R200 : RADEON_R100;
    gpu3d=(struct radeon3d_device){.engine=&test_engine,.heap={0x400000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.r200=r200,.tcl_bypass=!r200,.texture_bytes=512*1024};
    render3d_init(&scratch3d,&host3d,42,0,0,(struct render3d_extent){0});
    uint32_t context,handle,at=gpu3d.heap.first;
    CHECK(!render3d_create_context(&scratch3d,1,R3D_SOFTWARE,&context));
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
    uint32_t pixels[512];for(unsigned i=0;i<512;i++) pixels[i]=(i%32)<16 ? 0xff4080c0u : 0xffc04080u;
    for(unsigned i=0;i<4;i++) {
        CHECK(!render3d_create_resource(&scratch3d,1,context,kinds[i],i==2 ? 32 : 64,i==2 ? 16 : 64,i==2 ? pixels : 0,i==2 ? sizeof(pixels) : 0,&handle));
        struct render3d_resource *r=&scratch3d.contexts[0].resources[i];r->hardware=true;
        r->offset=i==2 ? gpu3d.heap.first+gpu3d.heap.bytes-gpu3d.texture_bytes : at;
        CHECK(!radeon3d_backend()->layout(&gpu3d,r));if(i!=2) at+=r->reserved;
    }
    gpu3d.consumer=&scratch3d;
    dropping3d=fault==1;corruption3d=fault==2 ? 1 : fault==3 ? 2 : 0;bad_restore3d=fault==4;
    fifo_stuck3d=fault==5 || fault==8;frozen3d=fault==8;cache_stuck3d=fault==6;read_delay3d=fault==7 ? 10 : 0;
    unsigned before=writes3d;int e=radeon3d_probe(&gpu3d);
    CHECK(fault ? e && !gpu3d.qualified && gpu3d.lost : !e && gpu3d.qualified && !gpu3d.lost);
    if(!fault) { CHECK(draw3d==3 && copy3d==3);CHECK(m.reg[0x1c3c/4]==0); }
    if(fault==5) CHECK(writes3d==before && m.us<=3*RADEON_WAIT_US+100);
    if(fault==8) CHECK(delay_calls3d && delay_calls3d<4*RADEON_POLLS && !m.us);
    CHECK(!render3d_close(&scratch3d));gpu3d.consumer=0;finish();
    printf("R%u guarded minimum fault=%u e=%d PASS\n",r200 ? 200 : 100,fault,e);
}
static void watchdog3d(void)
{
    setup3d(false,64,64);
    static struct render3d_request q; q=basic3d();q.count=R3D_VERTICES;
    for(unsigned i=3;i<R3D_VERTICES;i++) q.vertices[i]=q.vertices[i%3];
    struct render3d_resource *r[5];for(unsigned i=0;i<5;i++) r[i]=&scratch3d.contexts[0].resources[i];
    CHECK(!radeon3d_backend()->enter(&gpu3d));write_delay3d=5;
    int e=radeon3d_backend()->execute(&gpu3d,&q,r);write_delay3d=0;radeon3d_backend()->leave(&gpu3d);
    CHECK(e==-ETIMEDOUT && draw3d<256 && test_engine.cpu_safe && !gpu3d.lost);
    CHECK(m.reg[0x1c3c/4]==0 && m.reg[0x142c/4]==0 && m.reg[0x1428/4]==0);
    teardown3d();puts("continuously free FIFO: execution watchdog expires and state restores PASS");
}
static void fuzz3d(void)
{
    setup3d(false,64,64);
    struct render3d_resource *r[5];for(unsigned i=0;i<5;i++) r[i]=&scratch3d.contexts[0].resources[i];
    /* Shared stencil flags must never silently enter this D16 pipeline. */
    for (unsigned flag=R3D_STENCIL_TEST;flag<=R3D_CLEAR_STENCIL;flag<<=1) {
        struct render3d_request q=basic3d(); q.flags|=flag;
        unsigned words=writes3d,mem=memory3d;
        CHECK(radeon3d_backend()->validate(&gpu3d,&q,r)==-EINVAL);
        CHECK(writes3d==words && memory3d==mem);
    }
    uint32_t seed=0x47124001;
    for(unsigned i=0;i<5000;i++) {
        struct render3d_request q=basic3d();seed=seed*1664525+1013904223;
        switch(i%8) {
        case 0:q.flags|=128u<<(seed%24);break;
        case 1:q.count=R3D_VERTICES+1+seed%100;break;
        case 2:q.vertices[seed%3].rhw=0;break;
        case 3:q.vertices[seed%3].x=-1-(int)(seed%65536);break;
        case 4:q.state.sampler[seed%2].filter=5+seed%100;break;
        case 5:q.clear_depth=65536+seed%65536;break;
        case 6:q.state.depth_func=8+seed%100;break;
        default:q.vertices[seed%3].z=65536+seed%65536;break;
        }
        unsigned words=writes3d,mem=memory3d;
        CHECK(radeon3d_backend()->validate(&gpu3d,&q,r)<0);
        CHECK(writes3d==words && memory3d==mem);
    }
    struct render3d_request q=basic3d();uint32_t words[28];
    for(unsigned z=0;z<=65535;z++) {
        float expected=(float)((double)z/65535);uint32_t u;memcpy(&u,&expected,4);CHECK(radeon3d_depth_ieee(z)==u);
    }
    for(unsigned sample=0;sample<5000;sample++) {
        q=basic3d();q.state.flat=sample&1;q.flat_color[0]=seed;
        for(unsigned i=0;i<3;i++) {
            struct render3d_vertex *v=&q.vertices[i];
            seed=seed*1664525+1013904223;v->x=(seed%1025)*4096;
            seed=seed*1664525+1013904223;v->y=(seed%1025)*4096;
            seed=seed*1664525+1013904223;v->z=seed%65536;
            seed=seed*1664525+1013904223;v->rhw=256+seed%(16777216-255);
            seed=seed*1664525+1013904223;v->u=(int)(seed%(32*65536+1))-16*65536;
            seed=seed*1664525+1013904223;v->v=(int)(seed%(32*65536+1))-16*65536;
            q.uv1[i][0]=v->v;q.uv1[i][1]=v->u;
        }
        CHECK(!radeon3d_triangle_words(&q,0,sample&1,words));
        CHECK(words[0]==((sample&1) ? 0x30034u : 0x30134u));
        for(unsigned i=0;i<3;i++) {
            const struct render3d_vertex *v=&q.vertices[i];
            float expected[]={ (float)((double)v->x/65536),(float)((double)v->y/65536),
                (float)((double)v->z/65535),(float)((double)v->rhw/65536),0,
                (float)((double)v->u/65536),(float)((double)v->v/65536),
                (float)((double)q.uv1[i][0]/65536),(float)((double)q.uv1[i][1]/65536)};
            for(unsigned k=0;k<9;k++) {
                if(k==4) CHECK(words[1+i*9+k]==(q.state.flat ? q.flat_color[0] : v->color));
                else { uint32_t value;memcpy(&value,&expected[k],4);CHECK(words[1+i*9+k]==value); }
            }
        }
    }
    q=basic3d();
    CHECK(radeon3d_triangle_words(&q,UINT32_MAX,false,words)==-EINVAL);
    CHECK(radeon3d_signed_ieee(INT32_MIN)==0xc7000000u);
    for(unsigned i=0;i<3;i++) { CHECK(!radeon3d_triangle_words(&q,0,false,words));
        CHECK(ieee3d(words[1+i*9])==(long double)q.vertices[i].x/65536); }
    struct render3d_extent save=gpu3d.heap;gpu3d.heap.first+=4096;CHECK(radeon3d_backend()->validate(&gpu3d,&q,r)<0);gpu3d.heap=save;
    test_engine.group_id=RADEON_R300;CHECK(radeon3d_backend()->validate(&gpu3d,&q,r)<0);test_engine.group_id=RADEON_R100;
    teardown3d();printf("5000 validator mutations and 5000 independent vertex-stream cases, 65536 IEEE depth fixtures PASS\n");
}
static const struct render3d_family native_family3d={.status=R3D_UNQUALIFIED,.backend=radeon3d_backend};
static const struct display_driver native_driver3d={.render3d=&native_family3d};
const struct display_driver *display_driver_bound(void) { return &native_driver3d; }
const struct fb_device *fbdev_get(void)
{ static const struct fb_device fb={.present=true,.generation=42};return &fb; }
int radeon3d_owner_attach(struct render3d_service *s,const struct render3d_host *h,uint32_t gen)
{
    if(!radeon3d_backend()->qualified(&gpu3d,gen)) return -ENODEV;
    if(gpu3d.consumer) return -EBUSY;
    render3d_init(s,h,gen,radeon3d_backend(),&gpu3d,gpu3d.heap);gpu3d.consumer=s;return 0;
}
static void service3d(void)
{
    prepare(32);
    gpu3d=(struct radeon3d_device){.engine=&test_engine,.heap={0x400000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.texture_bytes=256*1024};
    uint32_t c,h[3];
    CHECK(!render3d_native_init(&scratch3d,&host3d));
    CHECK(render3d_create_context(&scratch3d,7,R3D_HARDWARE_REQUIRED,&c)==-EOPNOTSUPP);
    CHECK(!render3d_create_context(&scratch3d,7,R3D_AUTO,&c));CHECK(!scratch3d.contexts[0].hardware);
    CHECK(!render3d_close(&scratch3d));
    gpu3d.qualified=gpu3d.fullsize_qualified=gpu3d.perspective_qualified=true;gpu3d.mode_width=gpu3d.mode_height=64;
    CHECK(!render3d_native_init(&scratch3d,&host3d));
    struct ciuki_r3d_caps caps;radeon3d_backend()->query_caps(&gpu3d,&caps);
    CHECK(caps.features==RADEON3D_FEATURES && !(caps.features&(R3D_STENCIL_TEST|R3D_CLEAR_STENCIL)));
    CHECK(caps.max_size==64 && caps.max_vertices==768 && caps.sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR);
    CHECK(!render3d_create_context(&scratch3d,7,R3D_HARDWARE_REQUIRED,&c));
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_LINEAR_COLOR};
    for(unsigned i=0;i<3;i++) CHECK(!render3d_create_resource(&scratch3d,7,c,kinds[i],64,64,0,0,&h[i]));
    uint32_t textures[40],pixels[64*64];for(unsigned i=0;i<64*64;i++) pixels[i]=0xff8090a0;
    for(unsigned i=0;i<40;i++) CHECK(!render3d_create_resource(&scratch3d,7,c,R3D_TEXTURE,64,64,pixels,sizeof(pixels),&textures[i]));
    for(unsigned i=0;i<42;i++) {
        struct render3d_request q=basic3d();q.flags&=~R3D_TEXTURE1;q.target=h[0];q.depth=h[1];q.copy_target=h[2];q.texture=textures[i%40];
        struct render3d_fence f;CHECK(!render3d_submit(&scratch3d,7,c,&q,sizeof(q),&f));
        unsigned steps=0;while(scratch3d.queued) { CHECK(render3d_step(&scratch3d)==1 && ++steps<100); }
        CHECK(!render3d_wait(&scratch3d,7,c,f,0));CHECK(f.generation==42 && f.ordinal==i+1);
    }
    unsigned evicted=0;for(unsigned i=3;i<43;i++) evicted+=!scratch3d.contexts[0].resources[i].offset;
    CHECK(evicted && draw3d==42);CHECK(!render3d_close(&scratch3d));finish();
    printf("native attachment/capabilities and software-only unqualified chip; 42 generation fences and texture evictions=%u PASS\n",evicted);
}
int main(int argc,char **argv)
{
    setbuf(stdout,0);
    if(argc>2 && !strcmp(argv[1],"fullsize")) full_probe3d(atoi(argv[2]),640,480);
    else if(argc>1 && !strcmp(argv[1],"fullsize")) { for(unsigned r=0;r<2;r++) { full_probe3d(r,640,480);full_probe3d(r,800,600);full_probe3d(r,1024,768); } }
    else { for(unsigned r=0;r<2;r++) for(unsigned f=0;f<9;f++) minimum3d(r,f);watchdog3d();fuzz3d();service3d();differential3d(false);differential3d(true);formats_blends3d(false);formats_blends3d(true); }
    return 0;
}
