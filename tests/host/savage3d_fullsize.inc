/* Full-size boot gate and Quake-sized ordered lists on independent packets. */
#define FULL_CHECK(condition) do { CHECK(condition); if (failures) exit(1); } while (0)
static unsigned pressure_reads3d;
static uint32_t read_pressure3d(void *ctx,uint32_t reg)
{
    if (reg==SAVAGE_STATUS && m.fifo_used==32) m.fifo_drain=(++pressure_reads3d&1) ? 0 : 8;
    return model_read(ctx,reg);
}
static void fullsize_resources3d(struct render3d_service *s,const struct render3d_host *host,
                                 unsigned w,unsigned h,bool hardware)
{
    render3d_init(s,host,42,0,0,(struct render3d_extent){0});
    uint32_t c,handle; FULL_CHECK(!render3d_create_context(s,1,R3D_SOFTWARE,&c));
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,R3D_TEXTURE};
    unsigned at=gpu3d.heap.first,tex=gpu3d.heap.first+gpu3d.heap.bytes-SAVAGE3D_TEXTURE_MIN;
    for (unsigned i=0;i<5;i++) {
        if (kinds[i]==R3D_TEXTURE) {
            struct render3d_texture_desc t; texture_fixture3d(&t,R3D_ARGB8888,32,6);
            FULL_CHECK(!render3d_create_texture(s,1,c,&t,&handle));
        } else FULL_CHECK(!render3d_create_resource(s,1,c,kinds[i],w,h,0,0,&handle));
        if (hardware) {
            struct render3d_resource *r=&s->contexts[0].resources[i];
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? tex : at;
            FULL_CHECK(!savage3d_backend()->layout(&gpu3d,r));
            if (kinds[i]==R3D_TEXTURE) tex+=r->reserved; else at+=r->reserved;
        }
    }
}
static void fullsize3d(unsigned w,unsigned h,unsigned fault)
{
    prepare(32); m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d));
    draw3d=copy3d=writes3d=memory3d=count3d=corruption3d=0; dropping3d=bad_restore3d=false;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .mode_width=w,.mode_height=h,.memory={0,memory_read3d,memory_write3d,lock3d,unlock3d}};
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    FULL_CHECK(savage3d_fullsize_probe(&gpu3d,&reference3d)==-ENODEV && !writes3d);
    gpu3d.qualified=true; test_engine.render3d_qualified=true; gpu3d.fullsize_testing=true;
    fullsize_resources3d(&scratch3d,&host,w,h,true);
    fullsize_resources3d(&reference3d,&host,w,h,false);
    gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
    test_engine.io.write32=write_engine3d;
    m.exact_clock=m.fifo_model=true; m.fifo_drain=8;
    test_engine.io.read32=read_pressure3d; pressure_reads3d=0;
    if (fault==1) corruption3d=1;
    if (fault==2) corruption3d=2;
    if (fault==3) bad_restore3d=true;
    if (fault==4) gpu3d.heap.bytes-=4*1024*1024; /* preflight, before entry/writes */
    int e=savage3d_fullsize_probe(&gpu3d,&reference3d);
    if (e && !fault) fprintf(stderr,"fullsize %ux%u error=%d phase=%u expected=%08x actual=%08x channel=%u depth=%u\n",
        w,h,e,gpu3d.failed_phase,gpu3d.expected,gpu3d.actual,gpu3d.max_channel_error,gpu3d.max_depth_error);
    FULL_CHECK((e==0)==!fault && gpu3d.fullsize_qualified==!fault);
    if (!fault) FULL_CHECK(gpu3d.sample_count==4*w*h && gpu3d.max_channel_error<=2 && gpu3d.max_depth_error<=1);
    else if (fault==2 || fault==4) FULL_CHECK(!gpu3d.lost && gpu3d.qualified); /* safe refusal retains minimum */
    else FULL_CHECK(gpu3d.lost && test_engine.quarantined);
    if (fault==4) FULL_CHECK(!writes3d && !memory3d);
    FULL_CHECK(!locks && !count3d);
    if (!fault) {
        /* Layout refuses the unqualified rectangle, not just a square limit. */
        struct render3d_resource r={.kind=R3D_COLOR,.width=w,.height=h+1};
        FULL_CHECK(savage3d_backend()->layout(&gpu3d,&r)==-EOPNOTSUPP);
        struct ciuki_r3d_caps caps; savage3d_backend()->query_caps(&gpu3d,&caps);
        FULL_CHECK(caps.max_width==w && caps.max_height==h && caps.max_vertices==768 && caps.max_pixel_work==4*w*h);
        gpu3d.fullsize_qualified=false;
        savage3d_backend()->query_caps(&gpu3d,&caps);
        FULL_CHECK(caps.max_size==64 && caps.max_vertices==3 && !caps.sampling_features);
        FULL_CHECK(savage3d_backend()->layout(&gpu3d,&r)==-EOPNOTSUPP);
    }
    /* Reboot teardown permits freeing emulated quarantined memory. */
    FULL_CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d));
    gpu3d.consumer=0; FULL_CHECK(!allocations3d); finish();
    printf("fullsize %ux%u fault=%u samples=%u checksums=%08x/%08x PASS\n",w,h,fault,
        gpu3d.sample_count,gpu3d.hardware_checksum,gpu3d.reference_checksum);
}
static void projective3d(const uint32_t handles[2][4],const uint32_t contexts[2],uint32_t *pixels)
{
    static struct gl_hw_command command;
    static struct render3d_request q;
    static uint32_t golden[640*480]; static uint16_t golden_z[640*480];
    struct sw_texture texture={.used=1,.target=GL_TEXTURE_2D,.min=GL_LINEAR,.mag=GL_LINEAR,
        .wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT};
    texture.image[0]=(struct sw_image){.w=64,.h=64,.pixels=pixels,.internal=GL_RGBA};
    struct render3d_service *services[]={&scratch3d,&reference3d};
    struct ciuki_r3d_caps caps; savage3d_backend()->query_caps(&gpu3d,&caps);
    unsigned cases=0;
    for (unsigned pattern=0;pattern<2;pattern++) for (unsigned wrap=0;wrap<2;wrap++)
    for (unsigned rw=0;rw<2;rw++) for (unsigned winding=0;winding<2;winding++)
    for (unsigned shape=0;shape<4;shape++) {
        for (unsigned i=0;i<64*64;i++) pixels[i]=pattern ? (i&1 ? 0xffff00ffu : 0xff00ff00u) :
            0xff000000u|((i&63)*3<<16)|((i/64)*3<<8)|96;
        texture.wrap_s=texture.wrap_t=wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        command=(struct gl_hw_command){.state={.width=640,.height=480,.color=golden,.depth=golden_z,
            .color_mask=~0u,.depth_test=1,.depth_write=1,.depth_func=GL_LESS},
            .count=3,.clear_mask=GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,.clear_color=0xff102030,.clear_depth=65535};
        for (unsigned unit=0;unit<2;unit++) command.state.unit[unit]=(struct sw_unit){.enabled=1,.mode=GL_MODULATE,.texture=&texture};
        for (unsigned i=0;i<3;i++) {
            struct sw_vertex *v=&command.vertices[i];
            static const unsigned xy[4][3][2]={{{0,0},{128,0},{0,128}},
                {{0,0},{16,0},{0,16}},{{0,0},{512,0},{0,64}},{{32,0},{160,32},{0,120}}};
            *v=(struct sw_vertex){.x=xy[shape][i][0]*16,.y=xy[shape][i][1]*16,.z=1000,
                .color={255,255,255,255},.fog=255};
            for (unsigned unit=0;unit<2;unit++) {
                v->qw[unit]=(rw ? 2097152 : 524288)>>(rw ? i : 0);
                int u=i==1 ? 49152 : -16384,t=i==2 ? 49152 : -16384;
                v->uv[unit][0]=(int64_t)(unit ? t : u)*v->qw[unit]/1048576;
                v->uv[unit][1]=(int64_t)(unit ? u : t)*v->qw[unit]/1048576;
            }
        }
        if (winding) { struct sw_vertex tmp=command.vertices[1]; command.vertices[1]=command.vertices[2]; command.vertices[2]=tmp; }
        for (unsigned i=0;i<640*480;i++) { golden[i]=command.clear_color; golden_z[i]=65535; }
        sw_triangle(&command.state,command.vertices,command.vertices+1,command.vertices+2);
        for (unsigned side=0;side<2;side++) {
            struct render3d_resource *t=&services[side]->contexts[0].resources[2];
            memcpy(t->shadow,pixels,64*64*4); t->resident=false; /* fixture supplies a fresh immutable version */
            command.texture[0]=command.texture[1]=handles[side][2];
            FULL_CHECK(!gl_hw_encode(&caps,&command,handles[side][0],handles[side][1],handles[side][3],&q));
            struct render3d_fence fence;
            FULL_CHECK(!render3d_submit(services[side],7,contexts[side],&q,sizeof(q),&fence));
            while (services[side]->queued) FULL_CHECK(render3d_step(services[side])==1);
            FULL_CHECK(!render3d_wait(services[side],7,contexts[side],fence,0));
            uint32_t *image=services[side]->contexts[0].resources[0].shadow;
            uint16_t *z=services[side]->contexts[0].resources[1].shadow;
            for (unsigned i=0;i<640*480;i++) {
                FULL_CHECK((z[i]==65535)==(golden_z[i]==65535));
                FULL_CHECK(abs((int)z[i]-(int)golden_z[i])<=1);
                for (unsigned shift=0;shift<32;shift+=8) FULL_CHECK(abs((int)(image[i]>>shift&255)-(int)(golden[i]>>shift&255))<=2);
            }
        }
        /* Capability, nearest/mips, non-dyadic edges and alpha remain refusals. */
        caps.sampling_features=0; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        caps.sampling_features=CIUKI_R3D_PROJECTIVE_BILINEAR;
        texture.min=texture.mag=GL_NEAREST; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        texture.min=texture.mag=GL_LINEAR;
        texture.min=GL_LINEAR_MIPMAP_LINEAR; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q)); texture.min=GL_LINEAR;
        command.state.alpha_test=1; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q)); command.state.alpha_test=0;
        command.state.blend=1; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q)); command.state.blend=0;
        int saved_qw=command.vertices[0].qw[0];
        command.vertices[0].qw[0]=524272; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        command.vertices[0].qw[0]=saved_qw;
        command.vertices[1].x++; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        cases++;
    }
    FULL_CHECK(cases==64);
    puts("projective sampling: 64 GL-plane/scalar/independent-packet cases, contrasting texels, two stages, signed repeat/clamp, Q/W 0.5..2, both windings, dyadic area extrema and slanted edges, exact coverage and <=2 channel error PASS");
}
static void large_batch3d(void)
{
    prepare(32); m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d)); count3d=corruption3d=draw3d=copy3d=0;
    dropping3d=bad_restore3d=false;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .qualified=true,.fullsize_qualified=true,.mode_width=640,.mode_height=480,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d}};
    test_engine.render3d_qualified=true; test_engine.io.write32=write_engine3d;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    render3d_init(&scratch3d,&host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    render3d_init(&reference3d,&host,42,0,0,(struct render3d_extent){0});
    struct render3d_service *services[]={&scratch3d,&reference3d};
    uint32_t context[2],handles[2][4];
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
    uint32_t pixels[64*64];
    for (unsigned i=0;i<64*64;i++) pixels[i]=0xff000000u|((i&63)*3<<16)|((i/64)*3<<8)|96;
    for (unsigned side=0;side<2;side++) {
        FULL_CHECK(!render3d_create_context(services[side],7,side ? R3D_SOFTWARE : R3D_HARDWARE_REQUIRED,&context[side]));
        for (unsigned i=0;i<4;i++) FULL_CHECK(!render3d_create_resource(services[side],7,context[side],kinds[i],
            i==2 ? 64 : 640,i==2 ? 64 : 480,i==2 ? pixels : 0,i==2 ? sizeof(pixels) : 0,&handles[side][i]));
    }
    static struct render3d_request q;
    q=(struct render3d_request){.flags=R3D_FEATURES|R3D_TEXTURE1,.count=768,.clear_color=0xff102030,.clear_depth=65535};
    q.state.sampler[0]=q.state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,0,0};
    for (unsigned triangle=0;triangle<256;triangle++) for (unsigned v=0;v<3;v++) {
        unsigned i=triangle*3+v,x=(triangle%32)*16+(v==1 ? 16 : 0),y=(triangle/32)*16+(v==2 ? 16 : 0);
        q.vertices[i]=(struct render3d_vertex){x<<16,y<<16,1000,32768u<<v,0xffe0c0a0u,v==1 ? 49152 : 16384,v==2 ? 49152 : 16384};
        q.uv1[i][0]=q.vertices[i].v; q.uv1[i][1]=q.vertices[i].u;
    }
    m.fifo_model=true; m.exact_clock=true; m.fifo_drain=8;
    test_engine.io.read32=read_pressure3d; pressure_reads3d=0; m.fifo_used=m.fifo_peak=0;
    uint64_t before_packets=gpu3d.packets,before_waits=test_engine.fifo_waits,start=m.us;
    struct render3d_fence fence;
    unsigned rejected_writes=writes3d;
    q.target=handles[0][0]; q.depth=handles[0][1]; q.texture=q.texture1=handles[0][2]; q.copy_target=handles[0][3];
    q.count=R3D_VERTICES+3;
    FULL_CHECK(render3d_submit(&scratch3d,7,context[0],&q,sizeof(q),&fence)==-EINVAL);
    q.count=R3D_VERTICES;
    struct render3d_vertex saved[15]; memcpy(saved,q.vertices,sizeof(saved));
    for (unsigned i=0;i<15;i++) {
        q.vertices[i].x=i%3==1 ? 640<<16 : 0;
        q.vertices[i].y=i%3==2 ? 480<<16 : 0;
    }
    FULL_CHECK(render3d_submit(&scratch3d,7,context[0],&q,sizeof(q),&fence)==-EAGAIN);
    memcpy(q.vertices,saved,sizeof(saved));
    FULL_CHECK(writes3d==rejected_writes && !scratch3d.queued && !gpu3d.lost);
    for (unsigned side=0;side<2;side++) {
        q.target=handles[side][0]; q.depth=handles[side][1]; q.texture=q.texture1=handles[side][2]; q.copy_target=handles[side][3];
        FULL_CHECK(!render3d_submit(services[side],7,context[side],&q,sizeof(q),&fence));
        if (!side) { struct render3d_fence extra; FULL_CHECK(render3d_submit(services[side],7,context[side],&q,sizeof(q),&extra)==-EAGAIN); }
        unsigned steps=0;
        while (services[side]->queued) { FULL_CHECK(render3d_step(services[side])==1); FULL_CHECK(++steps<100000); }
        FULL_CHECK(!render3d_wait(services[side],7,context[side],fence,0));
    }
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
    for (unsigned i=0;i<4;i++) { hw[i]=&scratch3d.contexts[0].resources[i]; ref[i]=&reference3d.contexts[0].resources[i]; }
    for (unsigned i=0;i<640*480;i++) {
        uint32_t a=((uint32_t *)hw[0]->shadow)[i],b=((uint32_t *)ref[0]->shadow)[i];
        FULL_CHECK(a==((uint32_t *)hw[3]->shadow)[i]);
        FULL_CHECK((a==q.clear_color)==(b==q.clear_color));
        for (unsigned shift=0;shift<32;shift+=8) FULL_CHECK(abs((int)(a>>shift&255)-(int)(b>>shift&255))<=2);
        FULL_CHECK(abs((int)((uint16_t *)hw[1]->shadow)[i]-(int)((uint16_t *)ref[1]->shadow)[i])<=1);
    }
    FULL_CHECK(draw3d==256 && copy3d==1 && test_engine.fifo_waits>before_waits && m.fifo_peak<=32 && !m.fifo_used);
    /* Each batch has one event; state/clear/restore retain their small events. */
    FULL_CHECK(gpu3d.packets-before_packets==39);
    printf("large batch: 256 triangles/7168 words, one batch fence, FIFO peak=%u waits=%llu synthetic_us=%llu, scalar golden PASS\n",
        m.fifo_peak,(unsigned long long)(test_engine.fifo_waits-before_waits),(unsigned long long)(m.us-start));
    unsigned before=writes3d;
    uint32_t invalid[28]={0x80030008};
    FULL_CHECK(savage2d_render3d_batch(&test_engine,invalid,27)==-EINVAL && writes3d==before);
    invalid[0]=0x9601001e;
    FULL_CHECK(savage2d_render3d_batch(&test_engine,invalid,28)==-EINVAL && writes3d==before);
    m.fifo_model=false;
    projective3d(handles,context,pixels);
    FULL_CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d) && !allocations3d); finish();
}
static void fullsize_suite3d(void)
{
    fullsize3d(640,480,0); fullsize3d(800,600,0); fullsize3d(1024,768,0);
    for (unsigned fault=1;fault<=4;fault++) fullsize3d(640,480,fault);
    large_batch3d();
}
