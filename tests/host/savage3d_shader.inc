/* Round-3 sampled-alpha/blend oracle on large, independently sampled stages.
 * Host-only; GPL-2.0-only. No imported renderer implementation. */
static void shader_precision3d(void) {
    prepare(32); m.reg[0x48578/4]=0; m.exact_clock=true;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .qualified=true,.fullsize_qualified=true,.perspective_qualified=true,.texture_bytes=480*1024,
        .mode_width=640,.mode_height=480,.memory={0,memory_read3d,memory_write3d,lock3d,unlock3d}};
    test_engine.render3d_qualified=true; test_engine.io.write32=write_engine3d;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    render3d_init(&scratch3d,&host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    render3d_init(&reference3d,&host,42,0,0,(struct render3d_extent){0});
    struct render3d_service *services[]={&scratch3d,&reference3d};
    unsigned contexts[2],handles[2][5];
    for (unsigned side=0;side<2;side++) {
        FULL_CHECK(!render3d_create_context(services[side],7,side ? R3D_SOFTWARE : R3D_HARDWARE_REQUIRED,&contexts[side]));
        FULL_CHECK(!render3d_create_resource(services[side],7,contexts[side],R3D_COLOR,32,32,0,0,&handles[side][0]));
        FULL_CHECK(!render3d_create_resource(services[side],7,contexts[side],R3D_D16,32,32,0,0,&handles[side][1]));
        FULL_CHECK(!render3d_create_resource(services[side],7,contexts[side],R3D_LINEAR_COLOR,32,32,0,0,&handles[side][3]));
    }
    static uint32_t images[2][9][256*256];
    static struct gl_hw_command g;
    static struct render3d_request q;
    const unsigned sources[]={GL_ONE,GL_ZERO,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_COLOR,
        GL_ONE_MINUS_DST_COLOR,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    const unsigned destinations[]={GL_ONE,GL_ZERO,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_SRC_COLOR,
        GL_ONE_MINUS_SRC_COLOR,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    const unsigned modes[]={GL_MODULATE,GL_REPLACE,GL_ADD,GL_DECAL};
    struct ciuki_r3d_caps caps; savage3d_backend()->query_caps(&gpu3d,&caps);
    unsigned cases=0,ambiguous=0,mandatory=0;
    for (unsigned pattern=0;pattern<2;pattern++) {
        struct sw_texture textures[2]={{0}};
        for (unsigned stage=0;stage<2;stage++) {
            unsigned size=stage ? 128 : 256,levels=stage ? 8 : 9;
            struct render3d_texture_desc desc={.format=R3D_ARGB8888,.width=size,.height=size,.levels=levels};
            textures[stage]=(struct sw_texture){.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT,.mag=GL_LINEAR,.min=GL_LINEAR};
            for (unsigned l=0;l<levels;l++) {
                unsigned n=size>>l;
                for (unsigned y=0;y<n;y++) for (unsigned x=0;x<n;x++) {
                    unsigned a=pattern ? (x*7+y*13+l*31)&255 : ((x+y+l)&1)*255;
                    unsigned r=pattern ? x*255/n : ((x/2+y/3+l)&1)*255;
                    unsigned b=pattern ? y*255/n : ((x/3+y/2+l)&1)*255;
                    images[stage][l][y*n+x]=(a<<24)|(r<<16)|((255-r)<<8)|b;
                }
                desc.image[l]=(struct render3d_texture_image){images[stage][l],n*n*4,n*4};
                textures[stage].image[l]=(struct sw_image){.w=n,.h=n,.pixels=images[stage][l],.internal=GL_RGBA};
            }
            for (unsigned side=0;side<2;side++) FULL_CHECK(!render3d_create_texture(services[side],7,contexts[side],&desc,&handles[side][stage ? 4 : 2]));
        }
        for (unsigned filter=0;filter<3;filter++) for (unsigned pair=0;pair<64;pair++)
        for (unsigned alpha=0;alpha<3;alpha++) for (unsigned winding=0;winding<2;winding++) {
            g=(struct gl_hw_command){.state={.width=32,.height=32,.color_mask=~0u,
                .depth_test=1,.depth_write=1,.depth_func=GL_LESS,.blend=1,.sfactor=sources[pair/8],
                .dfactor=destinations[pair%8],.alpha_test=alpha!=0,.alpha_func=alpha==1 ? GL_GEQUAL : GL_GREATER,
                .alpha_ref=alpha==1 ? 128 : 0},.count=3,.clear_mask=GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,
                .clear_color=0x9362a4c8,.clear_depth=65535};
            for (unsigned stage=0;stage<2;stage++) {
                textures[stage].min=filter==0 ? GL_LINEAR : filter==1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
                textures[stage].wrap_s=textures[stage].wrap_t=stage && pattern && !filter ? GL_CLAMP : GL_REPEAT;
                textures[stage].border_color=0xff80a020;
                unsigned mode=pair==1 || pair==19 ? GL_MODULATE : modes[(pair+stage+pattern)%4];
                g.state.unit[stage]=(struct sw_unit){.enabled=1,.mode=(int)mode,.texture=&textures[stage]};
            }
            for (unsigned i=0;i<3;i++) {
                struct sw_vertex *v=g.vertices+i;
                *v=(struct sw_vertex){.x=i==1 ? 512 : 0,.y=i==2 ? 512 : 0,.z=1000,.color={96,160,224,255}};
                v->qw[0]=v->qw[1]=filter ? 1048576+(int)i*4096 : 524288u<<i;
                for (unsigned stage=0;stage<2;stage++) {
                    /* Large common scroll plus independent, signed local planes. */
                    int u=(stage ? -128 : 128)*65536+(i==1 ? 4*65536 : i==2 ? -2*65536 : 0)+1385;
                    int t=(stage ? 256 : -256)*65536+(i==2 ? 4*65536 : i==1 ? -2*65536 : 0)+3522;
                    if (textures[stage].wrap_s==GL_CLAMP) {
                        u=256+(i==1 ? 65024 : i==2 ? 32768 : 0);
                        t=256+(i==2 ? 65024 : i==1 ? 32768 : 0);
                    }
                    v->uv[stage][0]=(int64_t)u*v->qw[stage]/1048576;
                    v->uv[stage][1]=(int64_t)t*v->qw[stage]/1048576;
                }
            }
            if (winding) { struct sw_vertex v=g.vertices[1]; g.vertices[1]=g.vertices[2]; g.vertices[2]=v; }
            for (unsigned side=0;side<2;side++) {
                g.texture[0]=handles[side][2]; g.texture[1]=handles[side][4];
                FULL_CHECK(!gl_hw_encode(&caps,&g,handles[side][0],handles[side][1],handles[side][3],&q));
                struct render3d_fence f; FULL_CHECK(!render3d_submit(services[side],7,contexts[side],&q,sizeof(q),&f));
                while (services[side]->queued) FULL_CHECK(render3d_step(services[side])==1);
                FULL_CHECK(!render3d_wait(services[side],7,contexts[side],f,0));
                uint32_t *pixels=services[side]->contexts[0].resources[0].shadow;
                uint16_t *depth=services[side]->contexts[0].resources[1].shadow;
                for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) {
                    unsigned at=y*32+x;
                    if (!oracle_fragment3d(&g,x,y,pixels[at],depth[at],g.clear_color,65535)) {
                        fprintf(stderr,"shader case=%u side=%u pattern=%u filter=%u pair=%u alpha=%u winding=%u pixel=%u,%u colour=%08x depth=%u\n",
                            cases,side,pattern,filter,pair,alpha,winding,x,y,pixels[at],depth[at]);
                        FULL_CHECK(false);
                    }
                }
            }
            if (alpha==1) {
                long double lo[4],hi[4]; oracle_source3d(&g,4,28,lo,hi);
                unsigned ref=(unsigned)(lo[3]+hi[3])/2;
                g.state.alpha_ref=ref;
                if (lo[3]<ref && hi[3]>=ref) {
                    ambiguous++;
                    FULL_CHECK(oracle_fragment3d(&g,4,28,g.clear_color,65535,g.clear_color,65535));
                    FULL_CHECK(!oracle_fragment3d(&g,4,28,g.clear_color,997,g.clear_color,65535));
                }
                g.state.alpha_ref=0;
                mandatory++;
                FULL_CHECK(!oracle_fragment3d(&g,4,28,g.clear_color,65535,g.clear_color,65535));
            }
            cases++;
        }
        for (unsigned side=0;side<2;side++) for (unsigned stage=0;stage<2;stage++) FULL_CHECK(!render3d_destroy_resource(services[side],7,contexts[side],handles[side][stage ? 4 : 2]));
    }
    /* Native ARGB has a real destination alpha, unlike WGL's XRGB.
     * Exercise every legal selector pair against an independent scalar job. */
    const unsigned ns[]={R3D_ONE,R3D_ZERO,R3D_SRC_ALPHA,R3D_INV_SRC_ALPHA,R3D_DST_COLOR,
        R3D_INV_DST_COLOR,R3D_DST_ALPHA,R3D_INV_DST_ALPHA};
    const unsigned nd[]={R3D_ONE,R3D_ZERO,R3D_SRC_ALPHA,R3D_INV_SRC_ALPHA,R3D_SRC_COLOR,
        R3D_INV_SRC_COLOR,R3D_DST_ALPHA,R3D_INV_DST_ALPHA};
    for (unsigned pair=0;pair<64;pair++) {
        q=(struct render3d_request){.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY,
            .count=3,.clear_color=0x9362a4c8,.clear_depth=65535};
        q.state.blend=1; q.state.source=ns[pair/8]; q.state.destination=nd[pair%8];
        for (unsigned i=0;i<3;i++) q.vertices[i]=(struct render3d_vertex){i==1 ? 32*65536 : 0,
            i==2 ? 32*65536 : 0,1000,65536,0x8060a0c0,0,0};
        for (unsigned side=0;side<2;side++) {
            q.target=handles[side][0]; q.depth=handles[side][1]; q.copy_target=handles[side][3];
            struct render3d_fence f; FULL_CHECK(!render3d_submit(services[side],7,contexts[side],&q,sizeof(q),&f));
            while (services[side]->queued) FULL_CHECK(render3d_step(services[side])==1);
            FULL_CHECK(!render3d_wait(services[side],7,contexts[side],f,0));
        }
        FULL_CHECK(!memcmp(scratch3d.contexts[0].resources[0].shadow,reference3d.contexts[0].resources[0].shadow,32*32*4));
        FULL_CHECK(!memcmp(scratch3d.contexts[0].resources[1].shadow,reference3d.contexts[0].resources[1].shadow,32*32*2));
    }
    puts("native ARGB: 64 legal blend selector pairs, nonopaque destination alpha, exact scalar/packet colour and depth PASS");
    FULL_CHECK(ambiguous && mandatory && cases==2304);
    printf("sampled shader: %u differential cases, 64 blend pairs, four environments, alpha/mip thresholds, independent 256/128 stages, signed REPEAT/interior CLAMP, straddling=%u mandatory=%u PASS\n",cases,ambiguous,mandatory);
    FULL_CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d) && !allocations3d); finish();
}
