/* Round-4 native admission and independent/boot joint-colour regressions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../src/kernel/drivers/video/s3savage/savage3d_precision.h"
static void native_mip_bounds3d(void)
{
    prepare(32);
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .qualified=true,.fullsize_qualified=true,.perspective_qualified=true,.texture_bytes=480*1024,
        .mode_width=640,.mode_height=480,.memory={0,memory_read3d,memory_write3d,lock3d,unlock3d}};
    test_engine.render3d_qualified=true;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    render3d_init(&scratch3d,&host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    unsigned context,target,texture[2];
    FULL_CHECK(!render3d_create_context(&scratch3d,7,R3D_HARDWARE_REQUIRED,&context));
    FULL_CHECK(!render3d_create_resource(&scratch3d,7,context,R3D_COLOR,64,64,0,0,&target));
    for (unsigned i=0;i<2;i++) {
        struct render3d_texture_desc desc; texture_fixture3d(&desc,R3D_ARGB8888,i ? 256 : 64,i ? 9 : 7);
        FULL_CHECK(!render3d_create_texture(&scratch3d,7,context,&desc,&texture[i]));
    }
    static struct render3d_request q;
    for (unsigned stage=0;stage<2;stage++) for (unsigned filter=R3D_NEAREST_MIP;filter<=R3D_TRILINEAR;filter++)
    for (unsigned fault=0;fault<2;fault++) {
        q=(struct render3d_request){.target=target,.texture=texture[fault],.texture1=texture[fault],
            .flags=R3D_TEXTURED|R3D_TEXTURE1,.count=3};
        q.state.sampler[0]=q.state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,1,1};
        q.state.sampler[stage].filter=filter;
        for (unsigned i=0;i<3;i++) {
            q.vertices[i]=(struct render3d_vertex){i==1 ? 16*65536 : 0,i==2 ? (fault ? 4096 : 16*65536) : 0,
                1000,fault ? 65536 : i==1 ? 131072 : 32768,~0u,i==1 ? (fault ? 16*65536 : 57344) : 0,0};
            q.uv1[i][0]=q.vertices[i].u;
        }
        unsigned writes=writes3d,memory=memory3d;
        struct render3d_fence fence;
        FULL_CHECK(render3d_submit(&scratch3d,7,context,&q,sizeof(q),&fence)==-EOPNOTSUPP);
        FULL_CHECK(!scratch3d.queued && writes3d==writes && memory3d==memory && !gpu3d.lost);
        struct render3d_resource *r[R3D_REFS]={&scratch3d.contexts[0].resources[0],0,
            &scratch3d.contexts[0].resources[1+fault],0,&scratch3d.contexts[0].resources[1+fault]};
        /* Bilinear needs no LOD proof. A constant-W, well-shaped mip draw does. */
        q.state.sampler[stage].filter=R3D_BILINEAR;
        FULL_CHECK(!savage3d_backend()->validate(&gpu3d,&q,r));
        q.state.sampler[stage].filter=filter;
        for (unsigned i=0;i<3;i++) q.vertices[i].rhw=65536;
        q.vertices[2].y=16*65536;
        FULL_CHECK(!savage3d_backend()->validate(&gpu3d,&q,r));
        q.vertices[2]=q.vertices[1];
        FULL_CHECK(!savage3d_backend()->validate(&gpu3d,&q,r));
    }
    FULL_CHECK(!render3d_close(&scratch3d) && !allocations3d); finish();
    puts("PASS native mip admission: reciprocal-W and derivative-error bounds, both samplers/all mip filters, refusal before IO, constant/degenerate controls");
}
static uint32_t review_blend3d(uint32_t src,uint32_t dst,unsigned pair)
{
    uint32_t out=0;
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned c=src>>shift&255,d=dst>>shift&255,a=src>>24;
        unsigned sf=pair==1 ? a : pair==3 ? d : pair==4 ? 0 : 255;
        unsigned df=pair==0 ? 255 : pair==1 || pair==2 ? 255-a : pair==4 ? c : 0;
        unsigned value=(c*sf+d*df+127)/255;
        out|=(value>255 ? 255 : value)<<shift;
    }
    return out;
}
static void joint_layout3d(void)
{
    static uint32_t colours[64*64],white[64*64];
    static struct gl_hw_command g;
    static struct render3d_request q;
    struct sw_texture t[2]={{0}};
    struct render3d_resource target={.width=64,.height=64},tex[2]={{0}};
    struct render3d_resource *r[R3D_REFS]={&target,0,&tex[0],0,&tex[1]};
    const unsigned gs[]={GL_ONE,GL_SRC_ALPHA,GL_ONE,GL_DST_COLOR,GL_ZERO,GL_ONE};
    const unsigned gd[]={GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ZERO,GL_SRC_COLOR,GL_ZERO};
    const unsigned ns[]={R3D_ONE,R3D_SRC_ALPHA,R3D_ONE,R3D_DST_COLOR,R3D_ZERO,R3D_ONE};
    const unsigned nd[]={R3D_ONE,R3D_INV_SRC_ALPHA,R3D_INV_SRC_ALPHA,R3D_ZERO,R3D_SRC_COLOR,R3D_ZERO};
    for (unsigned alpha=0;alpha<2;alpha++) for (unsigned stage=0;stage<2;stage++) {
        for (unsigned i=0;i<64*64;i++) {
            colours[i]=i&1 ? 0xffff00ffu : (alpha ? 0x0000ff00u : 0xff00ff00u);
            white[i]=~0u;
        }
        g=(struct gl_hw_command){.state={.width=64,.height=64},.count=3,.clear_color=0xff204060};
        q=(struct render3d_request){.count=6,.clear_color=g.clear_color,.clear_depth=65535};
        for (unsigned unit=0;unit<2;unit++) {
            uint32_t *pixels=unit==stage ? colours : white;
            t[unit]=(struct sw_texture){.min=GL_LINEAR,.mag=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT};
            t[unit].image[0]=(struct sw_image){.w=64,.h=64,.pixels=pixels,.internal=GL_RGBA};
            tex[unit]=(struct render3d_resource){.width=64,.height=64,.levels=1,.shadow=pixels};
            g.state.unit[unit]=(struct sw_unit){.enabled=1,.mode=GL_MODULATE,.texture=&t[unit]};
            q.state.sampler[unit]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,1,1};
        }
        for (unsigned i=0;i<6;i++) {
            q.vertices[i]=(struct render3d_vertex){(i==1 || i==4 || i==5) ? 64*65536 : 0,
                i>=2 && i!=3 ? 64*65536 : 0,1000,65536,~0u,32799,32768};
            q.uv1[i][0]=32799; q.uv1[i][1]=32768;
            if (i<3) g.vertices[i]=(struct sw_vertex){.x=i==1 ? 1024 : 0,.y=i==2 ? 1024 : 0,
                .qw={1048576,1048576},.uv={{32799,32768},{32799,32768}},.color={255,255,255,255}};
        }
        uint32_t correct=alpha ? 0x78788778 : 0xff788778,fault=alpha ? 0x87788778 : 0xff877878;
        FULL_CHECK(oracle_pixel3d(&g,0,63,correct) && !oracle_pixel3d(&g,0,63,fault));
        FULL_CHECK(precision_pixel(&q,r,0,0,correct,1000,4096,8) && !precision_pixel(&q,r,0,0,fault,1000,4096,8));
        for (unsigned pair=0;pair<6;pair++) {
            g.state.blend=q.state.blend=1; g.state.sfactor=gs[pair]; g.state.dfactor=gd[pair];
            q.state.source=ns[pair]; q.state.destination=nd[pair];
            uint32_t layout_fault=alpha ? 0x78877878 : fault;
            uint32_t a=review_blend3d(correct,g.clear_color,pair),b=review_blend3d(layout_fault,g.clear_color,pair);
            bool good=oracle_pixel3d(&g,0,63,a),bad=oracle_pixel3d(&g,0,63,b);
            if (!good || bad) fprintf(stderr,"layout alpha=%u stage=%u pair=%u a=%08x b=%08x good=%d bad=%d\n",alpha,stage,pair,a,b,good,bad);
            FULL_CHECK(good && !bad);
            FULL_CHECK(precision_pixel(&q,r,0,0,a,1000,4096,8) && !precision_pixel(&q,r,0,0,b,1000,4096,8));
        }
        if (alpha) {
            g.state.blend=q.state.blend=0; g.state.alpha_test=q.state.alpha_test=1;
            g.state.alpha_func=GL_GEQUAL; q.state.alpha_func=R3D_GEQUAL;
            g.state.alpha_ref=q.state.alpha_ref=128;
            FULL_CHECK(!oracle_joint3d(&g,0,63,fault,g.clear_color,1));
            FULL_CHECK(!precision_pixel(&q,r,0,0,fault,1000,4096,8));
            FULL_CHECK(precision_pixel(&q,r,0,0,q.clear_color,65535,4096,8));
            FULL_CHECK(!precision_pixel(&q,r,0,0,q.clear_color,1000,4096,8));
        }
    }
    puts("PASS joint layout: green/magenta RG swap, shared RGBA sample in either stage, six blend pairs and correlated alpha threshold");
}
/* A trilinear pixel must not choose different normalized UVs in its two
 * mip levels. Blue encodes LOD; red/green encode the same U at two scales. */
static void joint_mip3d(void)
{
    static uint32_t images[64*64+32*32],white[64*64];
    for (unsigned i=0;i<64*64;i++) { images[i]=i&1 ? 0xffff0000 : 0xff000000; white[i]=~0u; }
    for (unsigned i=0;i<32*32;i++) images[64*64+i]=i&1 ? 0xff00ffff : 0xff0000ff;
    struct sw_texture texture={.min=GL_LINEAR_MIPMAP_LINEAR,.mag=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT};
    texture.image[0]=(struct sw_image){.w=64,.h=64,.pixels=images,.internal=GL_RGBA};
    texture.image[1]=(struct sw_image){.w=32,.h=32,.pixels=images+64*64,.internal=GL_RGBA};
    struct gl_hw_command *g=calloc(1,sizeof(*g)); FULL_CHECK(g);
    g->state.unit[0]=(struct sw_unit){.enabled=1,.mode=GL_MODULATE,.texture=&texture};
    struct oracle_position3d pos={.primary={255,255,255,255},.u={.5L},.v={.5L},.lambda={.5L}};
    long double box[6][2]; oracle_box3d(g,box);
    static struct render3d_request q;
    q.state.sampler[0]=(struct render3d_sampler){R3D_MODULATE,R3D_TRILINEAR,1,1};
    q.state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,1,1};
    struct render3d_resource tex={.width=64,.height=64,.levels=2,.shadow=images,.level_offset={0,64*64*4}};
    struct render3d_resource ones={.width=64,.height=64,.levels=1,.shadow=white};
    struct render3d_resource *r[R3D_REFS]={0,0,&tex,0,&ones};
    struct precision_position p={.uv={{8388608,8388608},{0,0}},.lod={128,0}};
    int32_t bounds[6][2]={{-4094,4094},{-4094,4094},{-14,14},{-4094,4094},{-4094,4094},{0,0}};
    for (unsigned fault=0;fault<2;fault++) {
        uint32_t colour=fault ? 0xff483c80 : 0xff404080;
        bool host=false,boot=false;
        for (unsigned mode=0;mode<10;mode++) {
            pos.arithmetic=(struct oracle_arithmetic3d){mode<4 ? 8 : mode<8 ? 12 : 0,(mode&2)!=0,(mode&1)!=0};
            p.arithmetic=(struct precision_arithmetic){pos.arithmetic.bits,pos.arithmetic.rounded,pos.arithmetic.separable};
            unsigned budget=1024;
            host|=oracle_fit3d(g,&pos,box,colour,0,0,3) || oracle_search3d(g,&pos,box,colour,0,0,&budget,0);
            budget=1024;
            boot|=precision_fit(&q,r,&p,bounds,colour,false,2) || precision_search(&q,r,&p,bounds,colour,false,&budget,0);
        }
        FULL_CHECK(host==!fault && boot==!fault);
    }
    free(g);
    puts("PASS joint trilinear witness: both mip levels share normalized UV; LOD-encoded independent-level colour fault refuses");
}
static void review4_suite3d(void) { native_mip_bounds3d(); joint_layout3d(); joint_mip3d(); }
