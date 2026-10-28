/* Register differential, qualification, hostile input and lifetime tests. */
#include "matrox3d_model.h"
static struct matrox3d_device device;
static struct matrox_engine engine;
static struct render3d_service hardware,reference;
static struct render3d_request request;
static unsigned allocations,cases;
static void *allocate(void *ctx,size_t bytes) { (void)ctx; void *p=calloc(1,bytes); if (p) allocations++; return p; }
static void release(void *ctx,void *p) { (void)ctx; if (p) { assert(allocations); allocations--; free(p); } }
static uint64_t clock_host(void *ctx) { (void)ctx; return model.time; }
static const struct render3d_host host={0,allocate,release,clock_host,0};
static void resources(struct render3d_service *s,unsigned w,unsigned h,bool hw,bool full)
{
    render3d_init(s,&host,42,0,0,(struct render3d_extent){0});
    uint32_t context,handle; assert(!render3d_create_context(s,1,R3D_SOFTWARE,&context));
    unsigned at=device.heap.first,tex=device.heap.first+device.heap.bytes-device.texture_bytes;
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,R3D_TEXTURE};
    for (unsigned i=0;i<5;i++) {
        if (kinds[i]==R3D_TEXTURE) {
            unsigned size=full ? (i==2 ? 256 : 128) : 32,levels=full ? (i==2 ? 9 : 8) : 6;
            struct render3d_texture_desc t={.format=R3D_ARGB8888,.width=size,.height=size,.levels=levels};
            uint32_t *p=malloc(size*size*8); assert(p); unsigned offset=0;
            for (unsigned level=0,n=size;level<levels;level++,n=n>1 ? n/2 : 1) {
                for (unsigned y=0;y<n;y++) for (unsigned x=0;x<n;x++)
                    p[offset+y*n+x]=0xff000000|((96+x*32/n+level*4)<<16)|((112+y*32/n+level*4)<<8)|(128+level*4+i);
                t.image[level]=(struct render3d_texture_image){p+offset,n*n*4,n*4}; offset+=n*n;
            }
            assert(!render3d_create_texture(s,1,context,&t,&handle)); free(p);
        } else assert(!render3d_create_resource(s,1,context,kinds[i],w,h,0,0,&handle));
        struct render3d_resource *r=&s->contexts[0].resources[i];
        if (hw) {
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? tex : at;
            assert(!matrox3d_backend()->layout(&device,r));
            if (kinds[i]==R3D_TEXTURE) tex+=r->reserved; else at+=r->reserved;
        }
    }
}
static void prepare(unsigned w,unsigned h,bool full)
{
    model_reset(); engine=model_engine();
    device=(struct matrox3d_device){.engine=&engine,.generation=42,.heap={model.first,model.bytes},
        .texture_bytes=512*1024,.encoding=1,.g400=true,.qualified=true,.fullsize_testing=true,.perspective_qualified=true,
        .mode_width=w,.mode_height=h,.memory={&model,model_read_memory,model_write_memory,model_enter,model_leave}};
    resources(&hardware,w,h,true,full); resources(&reference,w,h,false,full); device.consumer=&hardware;
}
static void finish(void)
{
    assert(!model.locks); assert(!render3d_close(&hardware)); assert(!render3d_close(&reference));
    assert(!allocations); free(model.ram); model.ram=0;
}
static void scene(void)
{
    memset(&request,0,sizeof(request)); request.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
    request.clear_color=0xff102030; request.clear_depth=65535; request.count=3;
    request.vertices[0]=(struct render3d_vertex){4<<16,5<<16,10000,65536,0xff204080,4096,4096};
    request.vertices[1]=(struct render3d_vertex){57<<16,12<<16,20000,65536,0xff80c040,49152,4096};
    request.vertices[2]=(struct render3d_vertex){11<<16,58<<16,30000,65536,0xffa04020,4096,49152};
    for (unsigned i=0;i<3;i++) { request.uv1[i][0]=request.vertices[i].v; request.uv1[i][1]=request.vertices[i].u; }
}
static void refs(struct render3d_service *s,struct render3d_resource *r[R3D_REFS])
{
    for (unsigned i=0;i<5;i++) r[i]=&s->contexts[0].resources[i];
    if (!(request.flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE))) r[1]=0;
    if (!(request.flags&R3D_TEXTURED)) r[2]=0;
    if (!(request.flags&R3D_COPY)) r[3]=0;
    if (!(request.flags&R3D_TEXTURE1)) r[4]=0;
}
static void compare_scene(void)
{
    const struct render3d_backend *b=matrox3d_backend();
    struct render3d_resource *hw[5],*ref[5]; refs(&hardware,hw); refs(&reference,ref);
    assert(!b->validate(&device,&request,hw));
    struct render3d_job job={0}; while (!render3d_scalar_slice(&request,ref,&job,&host)) {}
    assert(!b->enter(&device)); int e=b->execute(&device,&request,hw); b->leave(&device);
    if (e) fprintf(stderr,"execute error=%d phase=%u\n",e,device.failed_phase);
    assert(!e);
    for (unsigned i=0;i<64*64;i++) {
        uint32_t a=((uint32_t *)hw[0]->shadow)[i],c=((uint32_t *)ref[0]->shadow)[i];
        for (unsigned shift=0;shift<32;shift+=8) {
            int delta=(int)(a>>shift&255)-(int)(c>>shift&255);
            if (abs(delta)>2) {
                fprintf(stderr,"case=%u pixel=%u actual=%08x expected=%08x shift=%u\n",cases,i,a,c,shift);
                for (unsigned v=0;v<3;v++) fprintf(stderr,"vertex %u: Q4=%d,%d\n",v,request.vertices[v].x/4096,request.vertices[v].y/4096);
            }
            assert(abs(delta)<=2);
        }
        assert(a==((uint32_t *)hw[3]->shadow)[i]);
        int z=((uint16_t *)hw[1]->shadow)[i],rz=((uint16_t *)ref[1]->shadow)[i]; assert(abs(z-rz)<=1);
    }
    assert(engine.cpu_safe && !engine.quarantined);
    assert(model.reg[M_DSTORG/4]==0 && model.reg[M_PITCH/4]==engine.pitch && model.reg[M_MACCESS/4]==engine.maccess);
    assert(model.reg[M3D_TEXCTL2/4]==0 && model.reg[M3D_ALPHACTRL/4]==1); cases++;
}
static void differential(void)
{
    prepare(64,64,false); device.fullsize_testing=false; device.fullsize_qualified=true;
    for (unsigned encoding=0;encoding<2;encoding++) {
        device.encoding=encoding;
        for (unsigned flat=0;flat<2;flat++) for (unsigned depth=0;depth<8;depth++) if (depth!=R3D_NEVER) {
            scene(); request.state.flat=flat; request.flat_color[0]=0xff214387; request.state.depth_func=depth;
            compare_scene();
        }
        for (unsigned filter=0;filter<5;filter++) for (unsigned units=1;units<=2;units++) for (unsigned mode=0;mode<4;mode++) {
            scene(); request.flags|=R3D_TEXTURED|(units==2 ? R3D_TEXTURE1 : 0);
            for (unsigned stage=0;stage<units;stage++) request.state.sampler[stage]=(struct render3d_sampler){mode,filter,1,1};
            compare_scene();
        }
        for (unsigned alpha=0;alpha<8;alpha++) {
            scene(); request.state.alpha_test=1; request.state.alpha_func=alpha; request.state.alpha_ref=128;
            for (unsigned i=0;i<3;i++) request.vertices[i].color&=0x7fffffff;
            compare_scene();
        }
        for (unsigned src=0;src<10;src++) for (unsigned dst=0;dst<10;dst++) {
            if (src==R3D_SRC_COLOR || src==R3D_INV_SRC_COLOR || dst==R3D_DST_COLOR || dst==R3D_INV_DST_COLOR) continue;
            scene(); request.state.blend=1; request.state.source=src; request.state.destination=dst; compare_scene();
        }
        scene(); request.flags|=R3D_TEXTURED|R3D_TEXTURE1;
        request.vertices[0].rhw=32768; request.vertices[1].rhw=131072;
        request.state.sampler[0].filter=request.state.sampler[1].filter=R3D_BILINEAR;
        compare_scene();
    }
    printf("Matrox MMIO differential: %u flat/Gouraud, depth, alpha, blend, two-stage/filter/perspective cases PASS\n",cases);
    finish();
}
static void minimum(bool dropped,bool g400,unsigned reject_encoding)
{
    prepare(64,64,false); device.qualified=false; device.fullsize_testing=false;
    device.g400=g400; model.reject_encoding=reject_encoding;
    struct render3d_resource *t=&hardware.contexts[0].resources[2];
    /* Probe's documented texture is 32x16, not the differential's square. */
    struct render3d_resource saved=*t;
    t->height=16; t->levels=1; t->bytes=32*16*4; memset(t->level_offset,0,sizeof(t->level_offset));
    for (unsigned i=0;i<512;i++) ((uint32_t *)t->shadow)[i]=i%32<16 ? 0xff4080c0 : 0xffc04080;
    assert(!matrox3d_backend()->layout(&device,t)); model.no_draw=dropped;
    int e=matrox3d_probe(&device);
    if (!dropped && e) fprintf(stderr,"minimum e=%d phase=%u exp=%08x got=%08x\n",e,device.failed_phase,device.expected,device.actual);
    assert(dropped ? e==-EILSEQ && !device.qualified : !e && device.qualified);
    if (!dropped && reject_encoding==1) assert(device.encoding==0);
    assert(!device.lost && engine.cpu_safe && !model.locks); *t=saved; finish();
}
static void fullsize(unsigned w,unsigned h,bool g400)
{
    prepare(w,h,true); device.fullsize_testing=false; device.perspective_qualified=false;
    device.g400=g400;
    int e=matrox3d_fullsize_probe(&device,&reference);
    printf("Matrox %s fullsize %ux%u: e=%d phase=%u precision=%d/%u expected=%08x actual=%08x levels=%u/%u samples=%u\n",
        g400 ? "G400" : "G200",w,h,e,device.failed_phase,device.precision_error,device.precision_phase,device.expected,device.actual,
        device.measured_filter_levels[0],device.measured_filter_levels[1],device.sample_count);
    fflush(stdout);
    assert(!e && device.fullsize_qualified && !device.lost);
    if (!device.perspective_qualified) fprintf(stderr,"failed pixel %u,%u precision_samples=%u\n",device.precision_x,device.precision_y,device.precision_samples);
    if (!device.perspective_qualified) {
        struct render3d_request *q=(void *)hardware.ring[0];
        unsigned first=3,x=device.precision_x,y=device.precision_y;
        double weights[3]={1-(y+.5)/h,(x+.5)/w+(y+.5)/h-1,1-(x+.5)/w};
        for (unsigned stage=0;stage<2;stage++) {
            double uv[2]={0},den=0;
            for (unsigned i=0;i<3;i++) {
                double wgt=weights[i]*q->vertices[first+i].rhw;
                den+=wgt;
                uv[0]+=wgt*(stage ? q->uv1[first+i][0] : q->vertices[first+i].u)/65536.;
                uv[1]+=wgt*(stage ? q->uv1[first+i][1] : q->vertices[first+i].v)/65536.;
            }
            double u,v; model_coordinates(stage,x+.5,y+.5,&u,&v);
            fprintf(stderr,"stage %u uv actual=%.9f,%.9f expected=%.9f,%.9f\n",stage,u,v,uv[0]/den,uv[1]/den);
        }
    }
    assert(device.perspective_qualified); finish();
}
static void fuzz_and_faults(void)
{
    prepare(64,64,false); scene(); const struct render3d_backend *b=matrox3d_backend();
    struct render3d_resource *r[5]; refs(&hardware,r);
    uint32_t seed=0xabc;
    for (unsigned i=0;i<10000;i++) {
        seed=seed*1664525+1013904223; unsigned which=seed%8;
        uint32_t old;
        if (!which) { old=request.flags; request.flags=seed|0x80000000; }
        else if (which==1) { old=request.count; request.count=R3D_VERTICES+seed%1000+1; }
        else if (which==2) { old=r[0]->offset; r[0]->offset=seed|1; }
        else if (which==3) { old=request.vertices[0].x; request.vertices[0].x=-1; }
        else {
            old=0; request.flags|=R3D_TEXTURED; request.state.sampler[0].filter=R3D_TRILINEAR; r[2]=&hardware.contexts[0].resources[2];
            if (which==4) request.vertices[0].rhw=UINT32_MAX;
            if (which==5) request.vertices[0].u=INT32_MIN;
            if (which==6) request.uv1[0][1]=INT32_MAX;
            if (which==7) request.state.sampler[0].filter=UINT32_MAX;
        }
        unsigned writes=model.writes; assert(b->validate(&device,&request,r)<0 && writes==model.writes);
        if (!which) request.flags=old; else if (which==1) request.count=old;
        else if (which==2) r[0]->offset=old; else if (which==3) request.vertices[0].x=(int32_t)old;
        scene(); refs(&hardware,r);
    }
    struct matrox3d_trap traps[2]; unsigned n; assert(matrox3d_setup(&request,UINT32_MAX,0,traps,&n)==-EINVAL);
    device.g400=false; request.flags|=R3D_TEXTURED|R3D_TEXTURE1; refs(&hardware,r);
    assert(b->validate(&device,&request,r)==-EOPNOTSUPP); device.g400=true;
    finish();
    for (unsigned fault=0;fault<5;fault++) {
        prepare(64,64,false); scene(); refs(&hardware,r);
        if (fault==0) model.full=true;
        if (fault==1) model.wedge_launch=true;
        if (fault==2) model.dead=true;
        if (fault==3) { model.corrupt=true; model.read_delay=9; }
        if (fault==4) model.bad_restore=true;
        model.step=1;
        assert(!b->enter(&device)); int e=b->execute(&device,&request,r); b->leave(&device);
        assert(e<0 && !model.locks);
        if (fault!=3) assert(device.lost && engine.quarantined);
        else { assert(e==-EIO && engine.cpu_safe && device.guard_offset==model.first+508); b->quarantine(&device); }
        finish();
    }
    puts("Matrox validator: 10000 hostile records; FIFO/engine/dead MMIO/guard/restore faults and containment PASS");
}
static void packet_fuzz(void)
{
    prepare(64,64,false); device.fullsize_testing=false; device.fullsize_qualified=true;
    uint32_t seed=0x12345678;
    unsigned admitted=0,refused=0;
    for (unsigned i=0;i<512;i++) {
        scene(); request.state.flat=1; request.flat_color[0]=0xff5080a0;
        for (unsigned v=0;v<3;v++) {
            seed=seed*1664525+1013904223; request.vertices[v].x=(int32_t)(seed%1024)*4096;
            seed=seed*1664525+1013904223; request.vertices[v].y=(int32_t)(seed%1024)*4096;
            request.vertices[v].z=12345;
        }
        struct render3d_resource *r[R3D_REFS]; refs(&hardware,r);
        unsigned writes=model.writes;
        int e=matrox3d_backend()->validate(&device,&request,r);
        assert(writes==model.writes);
        if (e==-EOPNOTSUPP) { refused++; continue; }
        assert(!e); compare_scene(); admitted++;
    }
    finish(); printf("Matrox MMIO packet fuzz: %u admitted/%u refused subpixel traces, exact coverage/depth/copy and restored state PASS\n",admitted,refused);
}
static void formats(void)
{
    for (unsigned family=0;family<2;family++) {
        prepare(64,64,false); device.g400=family; device.fullsize_testing=false; device.fullsize_qualified=true;
        for (unsigned format=0;format<=R3D_A8;format++) {
            for (unsigned s=0;s<2;s++) {
                struct render3d_resource *t=&(s ? reference : hardware).contexts[0].resources[2];
                t->format=format; t->resident=false;
                uint32_t colour=format==R3D_A8 ? 0x80ffffffu : format==R3D_L8 ? 0xff707070u :
                    format==R3D_RGB565 ? 0xff428a21u : 0x8855aaccu;
                for (unsigned p=0;p<t->bytes/4;p++) ((uint32_t *)t->shadow)[p]=colour;
            }
            for (unsigned combine=0;combine<4;combine++) {
                if ((format==R3D_A8 && combine==R3D_DECAL) || (!family &&
                    (combine==R3D_ADD || (format==R3D_A8 && combine==R3D_REPLACE)))) continue;
                scene(); request.flags|=R3D_TEXTURED; request.state.sampler[0].combine=combine;
                compare_scene();
            }
        }
        finish();
    }
    puts("Matrox G200/G400: all six canonical texture formats and admitted combiners PASS");
}
int main(int argc,char **argv)
{
    if (argc>1 && !strcmp(argv[1],"fullsize")) { fullsize(640,480,true); fullsize(1024,768,true); fullsize(640,480,false); return 0; }
    differential(); formats(); minimum(false,true,0); minimum(false,true,1); minimum(false,false,0);
    minimum(true,true,0); fuzz_and_faults(); packet_fuzz(); return 0;
}
