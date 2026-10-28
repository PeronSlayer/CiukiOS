/* SVGA3D backend: service + strict host model + independent scalar
 * differential, qualification, fuzzing, containment and faults.
 * GPL-2.0-only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <ciuki/kernel.h>
#include "vmwaresvga3d_model.h"
#define M svga_model
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: check failed: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static struct render3d_service hardware,reference;
static struct render3d_request request;
static unsigned cases;
static const struct render3d_host host={0,svga_model_alloc,svga_model_free,svga_model_now,0};
static void resources(struct render3d_service *s,struct render3d_resource *r[R3D_REFS])
{ for (unsigned i=0;i<R3D_REFS;i++) r[i]=s->contexts[0].resources && s->contexts[0].resources[i].shadow ? &s->contexts[0].resources[i] : 0; }
static void define_context(void)
{
    const struct render3d_backend *b=vmsvga3d_backend();
    CHECK(!b->enter(&M.device)); CHECK(!vmsvga3d_context(&M.device,true)); b->leave(&M.device);
    CHECK(M.context || M.host==SVGA_HOST_VBOX_DX);
}
static void admit(uint32_t combine)
{
    M.device.qualified=M.device.minimum_passed=M.device.fullsize_qualified=M.device.perspective_qualified=true;
    M.device.combine=combine; define_context();
}
static void compare(unsigned width,unsigned height)
{
    uint32_t *a=hardware.contexts[0].resources[0].shadow,*b=reference.contexts[0].resources[0].shadow;
    uint16_t *az=hardware.contexts[0].resources[1].shadow,*bz=reference.contexts[0].resources[1].shadow;
    uint32_t *copy=hardware.contexts[0].resources[3].shadow;
    for (unsigned i=0;i<width*height;i++) {
        for (unsigned s=0;s<32;s+=8) {
            int delta=(int)(a[i]>>s&255)-(int)(b[i]>>s&255);
            if (delta< -2 || delta>2) { fprintf(stderr,"case %u at %u,%u shift %u hw=%08x ref=%08x\n",cases,i%width,i/width,s,a[i],b[i]); abort(); }
        }
        if (abs((int)az[i]-bz[i])>1) { fprintf(stderr,"case %u depth at %u: %u %u\n",cases,i,az[i],bz[i]); abort(); }
        if ((request.flags&R3D_COPY) && copy[i]!=a[i]) { fprintf(stderr,"case %u copy at %u\n",cases,i); abort(); }
    }
}
static void run_case(void)
{
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS]; resources(&hardware,hw); resources(&reference,ref);
    if (!(request.flags&R3D_TEXTURE1)) hw[4]=ref[4]=0;
    if (!(request.flags&R3D_TEXTURED)) hw[2]=ref[2]=0;
    if (!(request.flags&R3D_COPY)) hw[3]=ref[3]=0;
    if (!(request.flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE))) hw[1]=ref[1]=0;
    const struct render3d_backend *b=vmsvga3d_backend();
    int valid=b->validate(&M.device,&request,hw);
    if (valid) { fprintf(stderr,"case %u validate=%d\n",cases,valid); abort(); }
    CHECK(!b->enter(&M.device));
    int e=b->execute(&M.device,&request,hw); b->leave(&M.device);
    if (e) { fprintf(stderr,"case %u execute=%d phase=%u\n",cases,e,M.device.failed_phase); abort(); }
    CHECK(!svga_model_live() && !M.live_after_batch && M.max_live<=VMSVGA3D_SIDS && !M.locks);
    CHECK(M.engine.cpu_safe && !M.engine.quarantined && !M.regs[SVGA_REG_CONFIG_DONE]);
    struct render3d_job job={0}; while (!render3d_scalar_slice(&request,ref,&job,&host)) {}
    compare(hardware.contexts[0].resources[0].width,hardware.contexts[0].resources[0].height);
    cases++;
}
static void base_request(unsigned size)
{
    request=(struct render3d_request){.flags=VMSVGA3D_FEATURES,.count=3,.clear_color=0x80604020,.clear_depth=65535};
    for (unsigned i=0;i<3;i++) {
        request.vertices[i]=(struct render3d_vertex){i==1 ? (int32_t)size<<16 : 0,i==2 ? (int32_t)size<<16 : 0,
            16384,65536,0xc0806040,i==1 ? 65536 : 0,i==2 ? 65536 : 0};
        request.uv1[i][0]=request.vertices[i].v; request.uv1[i][1]=request.vertices[i].u;
    }
}
static void set_format(unsigned stage,unsigned format)
{
    for (unsigned side=0;side<2;side++) {
        struct render3d_resource *r=&(side ? reference : hardware).contexts[0].resources[stage ? 4 : 2];
        r->format=format; r->resident=false;
        for (unsigned l=0;l<r->levels;l++) {
            unsigned w=r->width>>l ? r->width>>l : 1,h=r->height>>l ? r->height>>l : 1;
            uint32_t *p=(void *)((uint8_t *)r->shadow+r->level_offset[l]);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
                uint32_t c=0x30000000u|((96+x*32/w)<<24)|((96+x*32/w+l*4)<<16)|((112+y*32/h)<<8)|(128+l*4);
                if (format==R3D_RGB565 || format==R3D_L8) c|=0xff000000u;
                if (format==R3D_A8) c|=0x00ffffffu;
                p[y*w+x]=c;
            }
        }
    }
}
static void differential(enum svga_host host_kind)
{
    svga_model_init(64,64,host_kind);
    uint32_t all=VMSVGA3D_COMBINE_REPLACE0|VMSVGA3D_COMBINE_REPLACE1|VMSVGA3D_COMBINE_ADD|VMSVGA3D_COMBINE_DECAL;
    admit(host_kind==SVGA_HOST_VMWARE ? all : VMSVGA3D_COMBINE_REPLACE0);
    svga_model_resources(&hardware,16,16,true,true); svga_model_resources(&reference,16,16,false,true);
    base_request(16);
    unsigned start=cases;
    for (unsigned filter=0;filter<5;filter++) for (unsigned mode=0;mode<4;mode++) {
        for (unsigned stage=0;stage<2;stage++) request.state.sampler[stage]=(struct render3d_sampler){mode,filter,1,1};
        if (host_kind!=SVGA_HOST_VMWARE && mode!=R3D_MODULATE) {
            /* Unqualified combiners refuse before any emission. */
            struct render3d_resource *hw[R3D_REFS]; resources(&hardware,hw);
            uint64_t fifo=M.fifo_writes;
            CHECK(vmsvga3d_backend()->validate(&M.device,&request,hw)==-EOPNOTSUPP && fifo==M.fifo_writes);
            if (mode!=R3D_REPLACE) continue;
            request.state.sampler[1].combine=R3D_MODULATE; /* stage-0 REPLACE is emulated */
        }
        run_case();
    }
    for (unsigned fmt=0;fmt<6;fmt++) for (unsigned fmt1=0;fmt1<6;fmt1++) {
        set_format(0,fmt); set_format(1,fmt1);
        for (unsigned mode=0;mode<4;mode++) {
            if (mode==R3D_DECAL && (fmt==R3D_A8 || fmt1==R3D_A8)) continue;
            for (unsigned stage=0;stage<2;stage++) request.state.sampler[stage]=(struct render3d_sampler){mode,R3D_BILINEAR,0,0};
            if (host_kind!=SVGA_HOST_VMWARE) {
                if (mode==R3D_REPLACE) request.state.sampler[1].combine=R3D_MODULATE;
                else if (mode!=R3D_MODULATE) {
                    bool alpha_add=mode==R3D_ADD && fmt==R3D_A8 && fmt1==R3D_A8;
                    if (!alpha_add) continue;
                }
            }
            run_case();
        }
    }
    set_format(0,R3D_ARGB8888); set_format(1,R3D_ARGB8888);
    request.state.sampler[0]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,0,0};
    request.state.sampler[1]=request.state.sampler[0];
    for (unsigned depth=0;depth<8;depth++) for (unsigned alpha=0;alpha<8;alpha++) {
        request.state.depth_func=depth; request.state.alpha_test=1; request.state.alpha_func=alpha; request.state.alpha_ref=100;
        run_case();
    }
    request.state.alpha_test=0;
    /* Equal-depth redraws separate LESS/LEQUAL/EQUAL/GEQUAL exactly. */
    for (unsigned depth=0;depth<8;depth++) {
        request.flags=VMSVGA3D_FEATURES; request.state.depth_func=R3D_LESS; run_case();
        request.flags=R3D_TEXTURED|R3D_TEXTURE1|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
        request.state.depth_func=depth;
        for (unsigned i=0;i<3;i++) request.vertices[i].color=0xff2040ffu;
        run_case();
        for (unsigned i=0;i<3;i++) request.vertices[i].color=0xc0806040;
    }
    request.flags=VMSVGA3D_FEATURES; request.state.depth_func=R3D_LESS;
    for (unsigned src=0;src<10;src++) for (unsigned dst=0;dst<10;dst++) {
        request.state.blend=1; request.state.source=src; request.state.destination=dst;
        if (!render3d_validate_state(&request)) run_case();
    }
    request.state.blend=0; request.state.flat=1; request.flat_color[0]=0xff80a0c0; run_case();
    request.state.flat=0;
    /* Write-only depth (ZFUNC ALWAYS) and test-only depth. */
    request.flags=VMSVGA3D_FEATURES&~R3D_DEPTH_TEST; run_case();
    request.flags=VMSVGA3D_FEATURES&~R3D_DEPTH_WRITE; run_case();
    request.flags=VMSVGA3D_FEATURES;
    for (unsigned i=0;i<3;i++) { request.vertices[i].rhw=32768u<<i; request.vertices[i].color=0xc0806040; }
    run_case();
    /* Varying W with non-uniform colour stays in software (affine colour contract). */
    request.vertices[1].color=0xff00ff00;
    { struct render3d_resource *hw[R3D_REFS]; resources(&hardware,hw);
      CHECK(vmsvga3d_backend()->validate(&M.device,&request,hw)==-EOPNOTSUPP); }
    for (unsigned i=0;i<3;i++) { request.vertices[i].rhw=65536; request.vertices[i].color=0xc0806040; }
    /* Untextured Gouraud and colour-only clears. */
    request.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
    for (unsigned i=0;i<3;i++) request.vertices[i].color=0xff000000u|(0xffu<<(i*8));
    run_case();
    request.flags=R3D_CLEAR_COLOR; request.count=0; memset(request.vertices,0,sizeof(request.vertices));
    memset(request.uv1,0,sizeof(request.uv1)); run_case();
    CHECK(!render3d_close(&hardware)); CHECK(!render3d_close(&reference)); svga_model_destroy();
    printf("PASS SVGA3D %s differential: %u cases (filters/mips, combiners, six formats, two stages, depth x alpha, blend pairs, flat, perspective, write/test-only depth, clears), zero surfaces left after every job\n",
        host_kind==SVGA_HOST_VMWARE ? "VMware-semantics" : "VirtualBox-GL-semantics",cases-start);
}
static void qualify(enum svga_host host_kind,unsigned width,unsigned height,bool gb)
{
    svga_model_init(width,height,host_kind);
    if (gb) {
        /* Guest-backed-object host: empty FIFO caps, SVGA_REG_DEV_CAP only. */
        const char *reason=0; M.gb_objects=true; CHECK(!svga_model_engine());
        M.locks=1; CHECK(!vmsvga3d_read_caps(&M.engine,&M.device.caps,&reason) && M.device.caps.gb_objects); M.locks=0;
        CHECK(M.dev_cap_reads==12 && !M.device.caps.hwversion);
    }
    M.device.qualification_started=1;
    svga_model_resources(&hardware,64,64,true,false); M.device.consumer=&hardware;
    CHECK(!vmsvga3d_backend()->qualified(&M.device,42));
    int e=vmsvga3d_probe(&M.device);
    if (host_kind==SVGA_HOST_VBOX_DX) {
        /* VGPU9 stubs: nothing renders; the probe refuses cleanly. */
        CHECK(e==-EILSEQ && !M.device.minimum_passed && !M.device.lost && M.engine.active && !M.engine.quarantined);
        CHECK(!svga_model_live());
        CHECK(!render3d_close(&hardware)); svga_model_destroy();
        puts("PASS SVGA3D VirtualBox DX-backend semantics: VGPU9 stubs refuse Q1 cleanly, 2D retained, no surface left");
        return;
    }
    if (e) fprintf(stderr,"probe error=%d phase=%u expected=%08x actual=%08x\n",e,M.device.failed_phase,M.device.expected,M.device.actual);
    CHECK(!e && M.device.minimum_passed && M.context);
    CHECK(!render3d_close(&hardware));
    M.device.fullsize_testing=true;
    svga_model_resources(&hardware,width,height,true,true); svga_model_resources(&reference,width,height,false,true);
    M.device.fullsize_testing=false; M.device.consumer=&hardware;
    e=vmsvga3d_fullsize_probe(&M.device,&reference);
    if (e || !M.device.perspective_qualified)
        fprintf(stderr,"qualification error=%d precision=%d phase=%u expected=%08x actual=%08x samples=%u\n",e,
            M.device.precision_error,M.device.precision_phase,M.device.expected,M.device.actual,M.device.precision_samples);
    CHECK(!e && M.device.fullsize_qualified && M.device.perspective_qualified);
    uint32_t all=VMSVGA3D_COMBINE_REPLACE0|VMSVGA3D_COMBINE_REPLACE1|VMSVGA3D_COMBINE_ADD|VMSVGA3D_COMBINE_DECAL;
    CHECK(M.device.combine==(host_kind==SVGA_HOST_VMWARE ? all : VMSVGA3D_COMBINE_REPLACE0));
    CHECK(!svga_model_live() && M.max_live<=VMSVGA3D_SIDS);
    /* Q4 belongs to the owner; the backend refuses until it is granted. */
    CHECK(!vmsvga3d_backend()->qualified(&M.device,42));
    M.device.qualified=true;
    CHECK(vmsvga3d_backend()->qualified(&M.device,42) && !vmsvga3d_backend()->qualified(&M.device,43));
    struct ciuki_r3d_caps caps; vmsvga3d_backend()->query_caps(&M.device,&caps);
    CHECK(caps.max_width==width && caps.max_height==height && caps.max_vertices==R3D_VERTICES && caps.max_pixel_work==4*width*height);
    CHECK(caps.max_texture_size==256 && caps.blend_factors==1023 && (caps.sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR));
    CHECK(caps.combine_modes==(host_kind==SVGA_HOST_VMWARE ? 15u : 3u));
    CHECK(!render3d_close(&hardware)); CHECK(!render3d_close(&reference)); svga_model_destroy();
    printf("PASS SVGA3D %s%s qualification %ux%u: Q1 (+MODULATE alpha), Q2 four mode-sized scenes, Q3 23 precision phases (%u samples), combiners %x, Q4 generation gate\n",
        host_kind==SVGA_HOST_VMWARE ? "VMware-semantics" : "VirtualBox-GL-semantics",gb ? " (GB objects, DEV_CAP)" : "",
        width,height,M.device.precision_samples,M.device.combine);
}
static void budget(void)
{
    svga_model_init(256,64,SVGA_HOST_VMWARE);
    svga_model_resources(&hardware,64,64,true,false); M.device.consumer=&hardware;
    CHECK(!vmsvga3d_probe(&M.device)); CHECK(!render3d_close(&hardware));
    M.device.fullsize_testing=true;
    svga_model_resources(&hardware,256,64,true,true); svga_model_resources(&reference,256,64,false,true);
    M.device.fullsize_testing=false; M.device.consumer=&hardware;
    M.device.qualification_started=1; M.clock+=VMSVGA3D_QUALIFY_US+10;
    uint64_t batches=M.batches;
    int e=vmsvga3d_fullsize_probe(&M.device,&reference);
    CHECK(e==-ETIMEDOUT && !M.device.lost && M.engine.active && !M.engine.quarantined);
    CHECK(!M.device.fullsize_qualified && batches==M.batches);
    CHECK(!render3d_close(&hardware)); CHECK(!render3d_close(&reference)); svga_model_destroy();
    puts("PASS SVGA3D qualification budget: clean refusal, no further batch, healthy 2D retained");
}
static void faults(unsigned variant)
{
    svga_model_init(64,64,SVGA_HOST_VMWARE);
    svga_model_resources(&hardware,64,64,true,false); M.device.consumer=&hardware;
    if (variant==0) M.hang=true;
    if (variant==1) M.wrong_fence=true;
    if (variant==2) M.corrupt_guard=true;
    if (variant==3) M.drop_draws=true;
    if (variant==4) M.stuck_clock=true,M.hang=true;
    int e=vmsvga3d_probe(&M.device);
    CHECK(e && !M.device.minimum_passed && !M.locks);
    if (variant==3) CHECK(e==-EILSEQ && !M.device.lost && M.engine.active && !M.engine.quarantined);
    else CHECK(M.device.lost && M.engine.quarantined && !M.engine.active);
    if (variant==0 || variant==4) CHECK(e==-ETIMEDOUT && M.delay_calls<=VMSVGA3D_DEFINE_WAIT_US/10u+2u);
    if (variant==1 || variant==2) CHECK(e==-EIO);
    CHECK(!vmsvga3d_backend()->qualified(&M.device,42));
    /* Neither a lost nor a refused device emits again. */
    uint64_t writes=M.fifo_writes,memory=M.memory_writes;
    struct render3d_resource *r[R3D_REFS]; resources(&hardware,r);
    const struct render3d_request *q=(void *)hardware.ring[0];
    if (M.device.lost) CHECK(vmsvga3d_backend()->enter(&M.device)==-EBUSY);
    else {
        CHECK(!vmsvga3d_backend()->enter(&M.device));
        CHECK(vmsvga3d_backend()->execute(&M.device,q,r)==-ENODEV);
        vmsvga3d_backend()->leave(&M.device);
    }
    CHECK(writes==M.fifo_writes && memory==M.memory_writes);
    CHECK(!render3d_close(&hardware)); svga_model_destroy();
    printf("PASS SVGA3D fault %u: error=%d; bounded wait, %s\n",variant,e,variant==3 ? "clean numeric refusal" : "both domains quarantined");
}
static void hosts(void)
{
    const char *reason=0; struct vmsvga3d_caps caps;
    svga_model_init(64,64,SVGA_HOST_QEMU);
    CHECK(vmsvga3d_read_caps(&M.engine,&caps,&reason)==-EOPNOTSUPP && !strcmp(reason,"no_svga3d") && !caps.valid);
    svga_model_destroy();
    struct { unsigned knob; const char *why; } refusals[]={{1,"hwversion"},{2,"caps_record"},{3,"devcap_limits"},
        {4,"no_fifo_fence"},{5,"fifo_registers"},{6,"devcap_limits"}};
    for (unsigned i=0;i<sizeof(refusals)/sizeof(*refusals);i++) {
        svga_model_init(64,64,SVGA_HOST_VMWARE);
        unsigned k=refusals[i].knob;
        if (k==1) M.hwversion=VMSVGA3D_HWVERSION_WS8_B1-1;
        if (k==2) M.bad_caps=true;
        if (k==3) M.max_texture_size=32;
        if (k==6) M.max_textures=0;
        CHECK(!svga_model_engine());
        if (k==4) M.engine.fifo_caps&=~SVGA_FIFO_CAP_FENCE;
        if (k==5) M.engine.min=4u*SVGA_FIFO_3D_HWVERSION_REVISED;
        uint64_t writes=M.fifo_writes+M.register_writes;
        CHECK(vmsvga3d_read_caps(&M.engine,&caps,&reason)==-EOPNOTSUPP && !strcmp(reason,refusals[i].why));
        CHECK(writes==M.fifo_writes+M.register_writes);
        svga_model_destroy();
    }
    svga_model_init(64,64,SVGA_HOST_VBOX_GL);
    CHECK(!vmsvga3d_read_caps(&M.engine,&caps,&reason) && caps.valid && !caps.texture_ops && caps.hwversion==VMSVGA3D_HWVERSION_WS8_B1);
    /* Actual pinned VirtualBox 2D caps omit legacy FILL. The 3D model's
     * synthetic FILL prerequisite isolates VGPU9 semantics; remove it to
     * prove that the production 2D gate refuses that real host profile. */
    M.caps_override=SVGA_CAP_RECT_COPY|SVGA_CAP_EXTENDED_FIFO|SVGA_CAP_3D;
    uint64_t fifo_before=M.fifo_writes;
    CHECK(svga_model_engine()==-EOPNOTSUPP && !M.engine.active && M.engine.cpu_safe &&
          !strcmp(M.engine.reason,"native_fill_unavailable") && fifo_before==M.fifo_writes);
    svga_model_destroy();
    svga_model_init(64,64,SVGA_HOST_VMWARE); M.gb_objects=true; M.max_textures=0; CHECK(!svga_model_engine());
    M.locks=1; CHECK(vmsvga3d_read_caps(&M.engine,&caps,&reason)==-EOPNOTSUPP && !strcmp(reason,"devcap_limits") && M.dev_cap_reads==12);
    M.locks=0; svga_model_destroy();
    puts("PASS SVGA3D host gates: QEMU (no SVGA_CAP_3D), hardware version, caps record bounds, devcap limits, FIFO fence/registers refuse with zero writes; VirtualBox GL devcaps parsed, real 2D caps refuse legacy FILL");
}
static unsigned random_word(unsigned *s) { *s=*s*1664525u+1013904223u; return *s; }
static void unsupported_features(void)
{
    svga_model_init(64,64,SVGA_HOST_VMWARE);
    admit(VMSVGA3D_COMBINE_REPLACE0);
    svga_model_resources(&hardware,64,64,true,true);
    struct render3d_resource *r[R3D_REFS]; resources(&hardware,r);
    const struct render3d_backend *b=vmsvga3d_backend();
    struct ciuki_r3d_caps caps; b->query_caps(&M.device,&caps);
    CHECK(b->features==VMSVGA3D_FEATURES && caps.features==VMSVGA3D_FEATURES);
    const unsigned stencil[]={R3D_STENCIL_TEST,R3D_CLEAR_STENCIL,R3D_STENCIL_TEST|R3D_CLEAR_STENCIL};
    for (unsigned i=0;i<3;i++) {
        base_request(64); request.flags|=stencil[i];
        uint64_t fifo=M.fifo_writes;
        CHECK(b->validate(&M.device,&request,r)==-EOPNOTSUPP);
        CHECK(b->execute(&M.device,&request,r)==-EOPNOTSUPP);
        CHECK(fifo==M.fifo_writes && !svga_model_live());
    }
    CHECK(!render3d_close(&hardware)); svga_model_destroy();
    puts("PASS SVGA3D unsupported stencil: test, clear and combined refuse with zero FIFO writes; no stencil capability");
}
static void fuzz(void)
{
    svga_model_init(64,64,SVGA_HOST_VMWARE);
    admit(VMSVGA3D_COMBINE_REPLACE0|VMSVGA3D_COMBINE_ADD);
    svga_model_resources(&hardware,64,64,true,true);
    struct render3d_resource *r[R3D_REFS]; resources(&hardware,r);
    const struct render3d_backend *b=vmsvga3d_backend();
    base_request(64);
    struct render3d_request *clean=malloc(sizeof(request)); CHECK(clean); *clean=request;
    unsigned seed=0x15ad0405,refused=0,accepted=0;
    for (unsigned i=0;i<20000;i++) {
        request=*clean; unsigned value=random_word(&seed);
        switch (i%14) {
        case 0: request.flags=value; break; case 1: request.count=value; break;
        case 2: request.vertices[0].x=(int32_t)value; break; case 3: request.vertices[0].rhw=value; break;
        case 4: request.vertices[0].u=(int32_t)value; break; case 5: request.state.source=value; break;
        case 6: request.state.sampler[0].filter=value; break; case 7: request.state.sampler[0].combine=value%8; break;
        case 8: request.clear_depth=value; break; case 9: request.vertices[0].z=value; break;
        case 10: request.state.alpha_ref=value; break; case 11: request.uv1[0][0]=(int32_t)value; break;
        case 12: request.state.sampler[1].combine=value%5; break;
        default: request.vertices[1].rhw=(value%4+1)*32768u; break;
        }
        uint64_t fifo=M.fifo_writes,memory=M.memory_writes;
        int e=b->validate(&M.device,&request,r);
        if (e) {
            refused++;
            CHECK(!b->enter(&M.device)); CHECK(b->execute(&M.device,&request,r)==e); b->leave(&M.device);
            CHECK(fifo==M.fifo_writes && memory==M.memory_writes);
        } else if (accepted<64) {
            /* Admitted mutations still decode strictly in the model. */
            CHECK(!b->enter(&M.device)); CHECK(!b->execute(&M.device,&request,r)); b->leave(&M.device);
            CHECK(!svga_model_live()); accepted++;
        }
    }
    request=*clean;
    for (unsigned i=0;i<2000;i++) {
        unsigned which=i%5; if (!r[which]) continue;
        struct render3d_resource saved=*r[which];
        r[which]->offset=random_word(&seed)&(i%2 ? ~0u : ~4095u); r[which]->reserved=random_word(&seed)&~4095u;
        uint64_t fifo=M.fifo_writes;
        CHECK(b->validate(&M.device,&request,r)==-EINVAL && fifo==M.fifo_writes);
        *r[which]=saved;
    }
    /* Encoder output is bounded and holds only the cited command ids. */
    unsigned words=0; CHECK(!vmsvga3d_encode(&M.device,&request,r,&words) && words<=VMSVGA3D_BATCH_WORDS);
    for (unsigned at=0;at<words;) {
        uint32_t id=M.device.commands[at]; CHECK(id>=VMSVGA3D_CMD_SURFACE_DEFINE && id<=VMSVGA3D_CMD_DRAW_PRIMITIVES);
        at+=2+M.device.commands[at+1]/4;
        CHECK(at<=words);
    }
    CHECK(refused>15000 && accepted);
    free(clean); CHECK(!render3d_close(&hardware)); svga_model_destroy();
    printf("PASS SVGA3D validator/encoder fuzz: 20000 states (%u refusals, %u admitted executed strictly), 2000 malformed extents, zero invalid emission\n",refused,accepted);
}
static void service(void)
{
    svga_model_init(64,64,SVGA_HOST_VMWARE);
    const struct render3d_backend *b=vmsvga3d_backend();
    struct render3d_extent heap={M.device.heap.first+VMSVGA3D_SCRATCH_BYTES,M.device.heap.bytes-VMSVGA3D_SCRATCH_BYTES};
    render3d_init(&hardware,&host,42,b,&M.device,heap);
    uint32_t c,t,z,tex,copy;
    CHECK(render3d_create_context(&hardware,7,R3D_HARDWARE_REQUIRED,&c)==-EOPNOTSUPP);
    CHECK(!render3d_create_context(&hardware,7,R3D_AUTO,&c) && !hardware.contexts[0].hardware);
    CHECK(!render3d_close(&hardware));
    admit(VMSVGA3D_COMBINE_REPLACE0);
    render3d_init(&hardware,&host,42,b,&M.device,heap);
    CHECK(!render3d_create_context(&hardware,7,R3D_AUTO,&c) && hardware.contexts[0].hardware);
    CHECK(!render3d_create_resource(&hardware,7,c,R3D_COLOR,16,16,0,0,&t));
    CHECK(!render3d_create_resource(&hardware,7,c,R3D_D16,16,16,0,0,&z));
    uint32_t pixels[16]; for (unsigned i=0;i<16;i++) pixels[i]=0xff4080c0;
    CHECK(!render3d_create_resource(&hardware,7,c,R3D_TEXTURE,4,4,pixels,sizeof(pixels),&tex));
    uint64_t draws=M.draws;
    for (unsigned flags=1;flags<=63;flags++) {
        request=(struct render3d_request){.flags=flags,.target=t,.clear_depth=65535,.count=3};
        if (flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) request.depth=z;
        if (flags&R3D_TEXTURED) request.texture=tex;
        if (flags&R3D_COPY) { CHECK(!render3d_create_resource(&hardware,7,c,R3D_LINEAR_COLOR,16,16,0,0,&copy)); request.copy_target=copy; }
        for (unsigned i=0;i<3;i++) request.vertices[i]=(struct render3d_vertex){i==1 ? 16<<16 : 0,i==2 ? 16<<16 : 0,16384,65536,0xffffffff,0,0};
        struct render3d_fence f; CHECK(!render3d_submit(&hardware,7,c,&request,sizeof(request),&f));
        while (hardware.queued) CHECK(render3d_step(&hardware)==1);
        CHECK(!render3d_wait(&hardware,7,c,f,0) && !svga_model_live());
        if (flags&R3D_COPY) CHECK(!render3d_destroy_resource(&hardware,7,c,request.copy_target));
    }
    CHECK(M.draws==draws+63 && hardware.contexts[0].hardware);
    /* ADD is unqualified here: the whole context retires to software, with
     * no further FIFO batch for it. */
    uint64_t batches=M.batches;
    request.state.sampler[0].combine=R3D_ADD; request.flags=R3D_TEXTURED|R3D_CLEAR_COLOR; request.depth=request.copy_target=0;
    struct render3d_fence f; CHECK(!render3d_submit(&hardware,7,c,&request,sizeof(request),&f));
    while (hardware.queued) CHECK(render3d_step(&hardware)==1);
    CHECK(!render3d_wait(&hardware,7,c,f,0) && M.batches==batches && !hardware.contexts[0].hardware);
    request.state.sampler[0].combine=R3D_MODULATE;
    CHECK(!render3d_submit(&hardware,7,c,&request,sizeof(request),&f));
    while (hardware.queued) CHECK(render3d_step(&hardware)==1);
    CHECK(!render3d_wait(&hardware,7,c,f,0) && M.batches==batches);
    CHECK(!render3d_close(&hardware)); svga_model_destroy();
    puts("PASS SVGA3D service: unqualified software, 63 flag combinations fenced on the model, refused ADD retires the whole context with zero batches");
}
static void eviction(void)
{
    svga_model_init(64,64,SVGA_HOST_VMWARE);
    M.device.texture_bytes=VMSVGA3D_TEXTURE_MIN;
    admit(0);
    struct render3d_extent heap={M.device.heap.first+VMSVGA3D_SCRATCH_BYTES,M.device.heap.bytes-VMSVGA3D_SCRATCH_BYTES};
    render3d_init(&hardware,&host,42,vmsvga3d_backend(),&M.device,heap);
    unsigned c,target,tex[8]; CHECK(!render3d_create_context(&hardware,7,R3D_HARDWARE_REQUIRED,&c));
    CHECK(!render3d_create_resource(&hardware,7,c,R3D_COLOR,16,16,0,0,&target));
    uint32_t *pixels=calloc(64*64,4); CHECK(pixels);
    for (unsigned i=0;i<8;i++) { for (unsigned p=0;p<64*64;p++) pixels[p]=0xff000000u|i*0x10203;
        CHECK(!render3d_create_resource(&hardware,7,c,R3D_TEXTURE,64,64,pixels,64*64*4,&tex[i])); }
    free(pixels);
    for (unsigned i=0;i<16;i++) {
        request=(struct render3d_request){.flags=R3D_TEXTURED|R3D_CLEAR_COLOR,.target=target,.texture=tex[i%8],.count=3};
        for (unsigned j=0;j<3;j++) request.vertices[j]=(struct render3d_vertex){j==1 ? 16<<16 : 0,j==2 ? 16<<16 : 0,0,65536,0xffffffff,0,0};
        struct render3d_fence f; CHECK(!render3d_submit(&hardware,7,c,&request,sizeof(request),&f));
        struct render3d_resource *r=&hardware.contexts[0].resources[1+i%8];
        CHECK(r->references==1);
        if (i==15) CHECK(!render3d_destroy_resource(&hardware,7,c,tex[i%8]));
        while (hardware.queued) CHECK(render3d_step(&hardware)==1);
        CHECK(!render3d_wait(&hardware,7,c,f,0));
        CHECK(((uint32_t *)hardware.contexts[0].resources[0].shadow)[0]==(0xff000000u|(i%8)*0x10203));
    }
    CHECK(!render3d_close(&hardware)); svga_model_destroy();
    puts("PASS SVGA3D 256 KiB pool: staged upload, LRU eviction/re-staging, queued pin and pending-free fence");
}
static int ratio_ok(uint64_t n,uint64_t d,uint32_t bits)
{
    if (!n) return bits==0;
    int e=(int)((bits>>23)&255)-127; uint64_t m=(bits&0x7fffff)|0x800000;
    /* value = m * 2^(e-23); compare 2*|n*2^k - d*m*2^(e-23+k)| <= d*2^(e-23+k) */
    int k=60-(e-23>0 ? e-23 : 0);
    __int128 lhs=(__int128)n<<(k>0 ? k : 0),rhs=(__int128)d*m;
    int shift=e-23+k; if (shift>=0) rhs<<=shift; else return -1;
    __int128 diff=lhs-rhs; if (diff<0) diff=-diff;
    __int128 half=((__int128)d<<shift)>>1;
    if (diff<half) return 1;
    if (diff==half) return !(m&1);
    return 0;
}
static void ieee(void)
{
    unsigned seed=0x3f800000,count=0;
    for (unsigned i=0;i<200000;i++) {
        uint64_t n=random_word(&seed)%(1u<<(1+i%31))+1,d=random_word(&seed)%(1u<<(1+i%24))+1;
        if (i%7==0) { n=random_word(&seed)&0xffff; d=65535; }
        if (i%11==0) { n=(int64_t)(random_word(&seed)%(1u<<26)); d=65536; }
        uint32_t bits=vmsvga3d_ratio_ieee(n,d,false);
        int ok=ratio_ok(n,d,bits);
        if (ok<0) continue;
        if (!ok) { fprintf(stderr,"ratio %llu/%llu -> %08x\n",(unsigned long long)n,(unsigned long long)d,bits); abort(); }
        float f; memcpy(&f,&bits,4); CHECK(fabsl((long double)f-(long double)n/d)<=(long double)n/d*1e-7L);
        count++;
    }
    CHECK(vmsvga3d_ratio_ieee(1,1,false)==0x3f800000u && vmsvga3d_ratio_ieee(1,2,true)==0xbf000000u);
    CHECK(vmsvga3d_fixed_ieee(-32768)==0xbf000000u && vmsvga3d_fixed_ieee(1024<<16)==0x44800000u && !vmsvga3d_fixed_ieee(0));
    uint32_t m[16]; vmsvga3d_projection(640,480,m);
    float p[16]; memcpy(p,m,sizeof(p));
    CHECK(p[0]==2.0f/640 && p[5]==-2.0f/480 && p[10]==1 && p[15]==1 && p[12]==-641.0f/640 && p[13]==481.0f/480);
    printf("PASS SVGA3D IEEE conversions: %u correctly rounded ratios (round-half-even), signed Q16, projection\n",count);
}
static void layouts(void)
{
    svga_model_init(640,480,SVGA_HOST_VMWARE);
    unsigned n=0;
    for (unsigned w=1;w<=1024;w*=2) for (unsigned h=1;h<=1024;h*=2) {
        struct render3d_resource r={.kind=R3D_TEXTURE,.width=w,.height=h,.pitch=w*4};
        unsigned ww=w,hh=h;
        do { r.level_offset[r.levels++]=r.bytes; r.bytes+=ww*hh*4; if (ww==1 && hh==1) break;
             ww=ww>1 ? ww/2 : 1; hh=hh>1 ? hh/2 : 1; } while (r.levels<11);
        CHECK(vmsvga3d_staging_bytes(&r)==((r.bytes+4095u)&~4095u));
        CHECK(!vmsvga3d_backend()->layout(&M.device,&r) && r.reserved==vmsvga3d_staging_bytes(&r)+8192u); n++;
    }
    struct render3d_resource color={.kind=R3D_COLOR,.width=640,.height=480,.pitch=2560,.bytes=2560*480};
    CHECK(vmsvga3d_backend()->layout(&M.device,&color)==-EOPNOTSUPP); /* not full-size qualified */
    M.device.fullsize_qualified=true;
    CHECK(!vmsvga3d_backend()->layout(&M.device,&color) && color.reserved==((2560u*480+4095u)&~4095u)+8192u);
    struct render3d_resource depth={.kind=R3D_D16,.width=641,.height=480,.pitch=1282,.bytes=1282*480};
    CHECK(vmsvga3d_backend()->layout(&M.device,&depth)==-EOPNOTSUPP);
    struct render3d_extent heap; uint32_t required;
    CHECK(!vmsvga3d_mode_heap(&M.engine,&M.fb,MODEL_VRAM_BYTES,&heap,&required));
    CHECK(heap.first==((640u*480*4+4095u)&~4095u) && heap.bytes==required+VMSVGA3D_HEAP_EXTRA);
    CHECK(vmsvga3d_mode_heap(&M.engine,&M.fb,640*480*4+required-4096,&heap,&required)==-ENOMEM);
    struct fb_device fb16=M.fb; fb16.bpp=16;
    CHECK(vmsvga3d_mode_heap(&M.engine,&fb16,MODEL_VRAM_BYTES,&heap,&required)==-EOPNOTSUPP);
    svga_model_destroy();
    printf("PASS SVGA3D layout: %u tight mip chains with guards, mode-gated targets, staging plan above the scanout lease\n",n);
}
int main(int argc,char **argv)
{
    const char *mode=argc>1 ? argv[1] : "differential";
    if (!strcmp(mode,"differential")) { differential(SVGA_HOST_VMWARE); differential(SVGA_HOST_VBOX_GL); }
    else if (!strcmp(mode,"qualification")) {
        qualify(SVGA_HOST_VMWARE,256,64,false); qualify(SVGA_HOST_VMWARE,640,480,false); qualify(SVGA_HOST_VMWARE,1024,768,false);
        qualify(SVGA_HOST_VMWARE,640,480,true);
        qualify(SVGA_HOST_VBOX_GL,256,64,false); qualify(SVGA_HOST_VBOX_GL,800,600,false); qualify(SVGA_HOST_VBOX_DX,256,64,false);
    }
    else if (!strcmp(mode,"budget")) budget();
    else if (!strcmp(mode,"fuzz")) fuzz();
    else if (!strcmp(mode,"unsupported")) unsupported_features();
    else if (!strcmp(mode,"service")) service();
    else if (!strcmp(mode,"eviction")) eviction();
    else if (!strcmp(mode,"hosts")) hosts();
    else if (!strcmp(mode,"ieee")) ieee();
    else if (!strcmp(mode,"layouts")) layouts();
    else if (!strcmp(mode,"fault") && argc>2) faults((unsigned)atoi(argv[2]));
    else { fprintf(stderr,"unknown mode %s\n",mode); return 2; }
    return 0;
}
