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
    read_delay3d=fault==5 ? 2 : fault==6 ? 9 : 0;
    int e=savage3d_fullsize_probe(&gpu3d,&reference3d);
    read_delay3d=0;
    if (e && !fault) fprintf(stderr,"fullsize %ux%u error=%d phase=%u expected=%08x actual=%08x channel=%u depth=%u\n",
        w,h,e,gpu3d.failed_phase,gpu3d.expected,gpu3d.actual,gpu3d.max_channel_error,gpu3d.max_depth_error);
    bool success=!fault || fault==5;
    FULL_CHECK((e==0)==success && gpu3d.fullsize_qualified==success);
    if (success) FULL_CHECK(gpu3d.sample_count==4*w*((h-1)/16+1+!!((h-1)%16)) &&
        gpu3d.max_channel_error<=2 && gpu3d.max_depth_error<=1);
    else if (fault==2 || fault==4 || fault==6) FULL_CHECK(!gpu3d.lost && gpu3d.qualified); /* safe refusal retains minimum */
    else FULL_CHECK(gpu3d.lost && test_engine.quarantined);
    if (fault==3) FULL_CHECK(m.fault_count==1); /* execute + caller report one loss */
    if (fault==5 || fault==6) {
        FULL_CHECK(test_engine.cpu_safe && !test_engine.event_pending && !test_engine.quarantined);
        FULL_CHECK(!m.reg[0x850c/4] && !m.reg[0x48c40/4] && !m.reg[0x48584/4]);
        FULL_CHECK(fault==5 ? gpu3d.fullsize_us>20000 : e==-ETIMEDOUT && gpu3d.failed_phase==8);
    }
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
    for (unsigned pattern=0;pattern<3;pattern++) for (unsigned wrap=0;wrap<2;wrap++)
    for (unsigned rw=0;rw<2;rw++) for (unsigned winding=0;winding<2;winding++)
    for (unsigned shape=0;shape<4;shape++) {
        for (unsigned i=0;i<64*64;i++) pixels[i]=pattern==2 ? (((i%64)^(i/64))&1 ? 0xffffffffu : 0) :
            pattern ? (i&1 ? 0xffff00ffu : 0xff00ff00u) :
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
                v->qw[unit]=rw ? 2097152 : 524288;
                int x=v->x/16,y=v->y/16;
                int u=unit ? 8192-x*32+y*128 : -16384+x*128+y*64;
                int t=unit ? 16384+x*64-y*128 : -8192+x*32+y*256;
                v->uv[unit][0]=(int64_t)u*v->qw[unit]/1048576;
                v->uv[unit][1]=(int64_t)t*v->qw[unit]/1048576;
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
        command.vertices[1].uv[1][0]+=2; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        command.vertices[1].uv[1][0]-=2;
        saved_qw=command.vertices[1].qw[0];
        command.vertices[1].qw[0]=command.vertices[1].qw[1]=1048576;
        FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        command.vertices[1].qw[0]=command.vertices[1].qw[1]=saved_qw;
        command.vertices[1].x++; FULL_CHECK(gl_hw_encode(&caps,&command,1,2,3,&q));
        cases++;
    }
    FULL_CHECK(cases==96);
    puts("varying sampling: 96 GL-plane/scalar/packet cases, 2D checkerboards, independent stage UV, constant W, integral planes, <=2 channel error PASS");
}
static void checkerboard_refusal3d(const uint32_t handles[2][4],const uint32_t contexts[2],uint32_t *pixels)
{
    static struct gl_hw_command command;
    static struct render3d_request q;
    static uint32_t golden[64*64];
    struct sw_texture texture={.used=1,.target=GL_TEXTURE_2D,.min=GL_LINEAR,.mag=GL_LINEAR,
        .wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT};
    texture.image[0]=(struct sw_image){.w=64,.h=64,.pixels=pixels,.internal=GL_RGBA};
    for (unsigned y=0;y<64;y++) for (unsigned x=0;x<64;x++) pixels[y*64+x]=(x^y)&1 ? ~0u : 0;
    command=(struct gl_hw_command){.state={.width=64,.height=64,.color=golden,.color_mask=~0u},.count=3};
    static const int uv[2][3][2]={{{-5362,-1766},{13308,-22958},{-23524,13486}},
        {{18450,-4344},{-17516,26844},{-31274,16628}}};
    for (unsigned unit=0;unit<2;unit++)
        command.state.unit[unit]=(struct sw_unit){.enabled=1,.mode=GL_MODULATE,.texture=&texture};
    for (unsigned i=0;i<3;i++) {
        struct sw_vertex *v=&command.vertices[i];
        *v=(struct sw_vertex){.x=i==1 ? 16*16 : 0,.y=i==2 ? 16*16 : 0,.color={255,255,255,255}};
        for (unsigned unit=0;unit<2;unit++) {
            v->qw[unit]=524288;
            memcpy(v->uv[unit],uv[unit][i],sizeof(v->uv[unit]));
        }
    }
    sw_triangle(&command.state,command.vertices,command.vertices+1,command.vertices+2);
    const unsigned sample=58*64;
    FULL_CHECK(golden[sample]==0xeaeaeaeau);
    struct ciuki_r3d_caps caps; savage3d_backend()->query_caps(&gpu3d,&caps);
    FULL_CHECK(gl_hw_encode(&caps,&command,1,0,0,&q));
    /* Reproduce the reviewed packet explicitly, solely to prove that this
     * fixture exceeds tolerance when the admission predicate is bypassed. */
    q=(struct render3d_request){.flags=R3D_CLEAR_COLOR|R3D_TEXTURED|R3D_TEXTURE1,.count=3};
    q.state.sampler[0]=q.state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,1,1};
    for (unsigned i=0;i<3;i++) {
        q.vertices[i]=(struct render3d_vertex){command.vertices[i].x*4096,
            (64*16-command.vertices[i].y)*4096,0,32768,~0u,uv[0][i][0]*2,uv[0][i][1]*2};
        q.uv1[i][0]=uv[1][i][0]*2; q.uv1[i][1]=uv[1][i][1]*2;
    }
    struct render3d_service *services[]={&scratch3d,&reference3d};
    for (unsigned side=0;side<2;side++) {
        uint32_t target;
        FULL_CHECK(!render3d_create_resource(services[side],7,contexts[side],R3D_COLOR,64,64,0,0,&target));
        struct render3d_resource *texture_resource=&services[side]->contexts[0].resources[2];
        memcpy(texture_resource->shadow,pixels,64*64*4); texture_resource->resident=false;
        q.target=target; q.texture=q.texture1=handles[side][2];
        struct render3d_fence fence;
        FULL_CHECK(!render3d_submit(services[side],7,contexts[side],&q,sizeof(q),&fence));
        while (services[side]->queued) FULL_CHECK(render3d_step(services[side])==1);
        FULL_CHECK(!render3d_wait(services[side],7,contexts[side],fence,0));
        uint32_t image[64*64];
        FULL_CHECK(!render3d_readback(services[side],7,contexts[side],target,image,sizeof(image)));
        if (!side) FULL_CHECK(image[sample]==0xededededu);
        if (!side) FULL_CHECK(abs((int)(image[sample]&255)-(int)(golden[sample]&255))>2);
        FULL_CHECK(!render3d_destroy_resource(services[side],7,contexts[side],target));
    }
    puts("checkerboard counterexample: GL eaeaeaea, packet edededed at (0,58); production encoder refuses PASS");
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
    checkerboard_refusal3d(handles,context,pixels);
    /* Ordinary jobs need every row, unlike the boot checksum. Slow completed
     * reads may exceed 20 ms in total while each CPU chunk stays bounded. */
    q=(struct render3d_request){.target=handles[0][0],.copy_target=handles[0][3],.flags=R3D_COPY};
    read_delay3d=2;
    FULL_CHECK(!render3d_submit(&scratch3d,7,context[0],&q,sizeof(q),&fence));
    FULL_CHECK(render3d_step(&scratch3d)==1 && !scratch3d.queued);
    FULL_CHECK(!render3d_wait(&scratch3d,7,context[0],fence,0));
    FULL_CHECK(!m.reg[0x850c/4] && !m.reg[0x48c40/4] && !m.reg[0x48584/4]);
    read_delay3d=9;
    FULL_CHECK(!render3d_submit(&scratch3d,7,context[0],&q,sizeof(q),&fence));
    FULL_CHECK(render3d_step(&scratch3d)==-ETIMEDOUT && !scratch3d.queued);
    read_delay3d=0;
    FULL_CHECK(render3d_wait(&scratch3d,7,context[0],fence,0)==-ETIMEDOUT);
    FULL_CHECK(!scratch3d.quarantined && !gpu3d.lost && gpu3d.fullsize_qualified &&
        test_engine.cpu_safe && !test_engine.event_pending && !test_engine.quarantined);
    FULL_CHECK(!m.reg[0x850c/4] && !m.reg[0x48c40/4] && !m.reg[0x48584/4]);
    uint32_t fresh_context,fresh_target;
    FULL_CHECK(!render3d_create_context(&scratch3d,7,R3D_HARDWARE_REQUIRED,&fresh_context));
    FULL_CHECK(!render3d_create_resource(&scratch3d,7,fresh_context,R3D_COLOR,16,16,0,0,&fresh_target));
    q=(struct render3d_request){.target=fresh_target,.flags=R3D_CLEAR_COLOR,.clear_color=0xff123456};
    FULL_CHECK(!render3d_submit(&scratch3d,7,fresh_context,&q,sizeof(q),&fence));
    FULL_CHECK(render3d_step(&scratch3d)==1 && !render3d_wait(&scratch3d,7,fresh_context,fence,0));
    puts("bounded CPU readback: 2 us/read completes and restores; 9 us/read refuses and restores without quarantine PASS");
    FULL_CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d) && !allocations3d); finish();
}
static void fullsize_suite3d(void)
{
    fullsize3d(640,480,0); fullsize3d(800,600,0); fullsize3d(1024,768,0);
    for (unsigned fault=1;fault<=6;fault++) fullsize3d(640,480,fault);
    large_batch3d();
}
