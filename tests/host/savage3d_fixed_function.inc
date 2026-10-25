/* Host golden/equivalence fixtures. Coverage must match exactly; channels may
 * differ by two levels and normalized D16 by one, as predeclared in the
 * contract. Scalar output itself is deterministic. No tolerance for a hole,
 * guard error, wrong occlusion or address. SPDX-License-Identifier: GPL-2.0-only */
static struct render3d_service reference3d;
static uint8_t images3d[R3D_MIPS][64*64*4+64*3];
static void texture_fixture3d(struct render3d_texture_desc *t,unsigned format,unsigned side,unsigned levels)
{
    memset(t,0,sizeof(*t)); t->format=format; t->width=t->height=side; t->levels=levels;
    unsigned bpp=format<2 ? 4 : format<4 ? 2 : 1,w=side;
    for (unsigned level=0;level<levels;level++) {
        unsigned pitch=w*bpp+3;
        t->image[level]=(struct render3d_texture_image){images3d[level],pitch*w,pitch};
        for (unsigned y=0;y<w;y++) for (unsigned x=0;x<w;x++) {
            unsigned a=96+level*12,r=32+x*2+level*8,g=48+y*2+level*8,b=64+level*12;
            uint32_t c=a<<24|r<<16|g<<8|b;
            uint8_t *dst=images3d[level]+y*pitch+x*bpp;
            if (format==R3D_RGBA8888) { dst[0]=r; dst[1]=g; dst[2]=b; dst[3]=a; }
            else {
                if (format==R3D_RGB565) c=(r>>3)<<11|(g>>2)<<5|b>>3;
                else if (format==R3D_ARGB4444) c=(a>>4)<<12|(r>>4)<<8|(g>>4)<<4|b>>4;
                else if (format==R3D_L8) c=64+level*8+x;
                else if (format==R3D_A8) c=a;
                memcpy(dst,&c,bpp);
            }
        }
        w=w>1 ? w/2 : 1;
    }
}
static void equivalent_textures3d(const struct render3d_host *host,struct render3d_request *q,
    const struct render3d_texture_desc *tex,const struct render3d_texture_desc *tex1,
    bool software,unsigned case_id,const uint32_t *expected)
{
    uint32_t c[2],h[2][5];
    struct render3d_service *services[]={&scratch3d,&reference3d};
    CHECK(!render3d_create_context(services[0],9,software ? R3D_AUTO : R3D_HARDWARE_REQUIRED,&c[0]));
    CHECK(!render3d_create_context(services[1],9,R3D_SOFTWARE,&c[1]));
    for (unsigned side=0;side<2;side++) {
        CHECK(!render3d_create_resource(services[side],9,c[side],R3D_COLOR,16,16,0,0,&h[side][0]));
        CHECK(!render3d_create_resource(services[side],9,c[side],R3D_D16,16,16,0,0,&h[side][1]));
        if (tex) CHECK(!render3d_create_texture(services[side],9,c[side],tex,&h[side][2]));
        if (tex1) CHECK(!render3d_create_texture(services[side],9,c[side],tex1,&h[side][4]));
        CHECK(!render3d_create_resource(services[side],9,c[side],R3D_LINEAR_COLOR,16,16,0,0,&h[side][3]));
        q->target=h[side][0]; q->depth=h[side][1]; q->copy_target=h[side][3];
        q->texture=tex ? h[side][2] : 0;
        q->texture1=q->flags&R3D_TEXTURE1 ? (tex1 ? h[side][4] : q->texture) : 0;
        struct render3d_fence f; unsigned before=writes3d;
        CHECK(!render3d_submit(services[side],9,c[side],q,sizeof(*q),&f));
        unsigned steps=0;
        while (services[side]->queued) { CHECK(render3d_step(services[side])==1); CHECK(++steps<100); }
        CHECK(!render3d_wait(services[side],9,c[side],f,0));
        if (!side && software) CHECK(writes3d==before);
        if (!side && !software) CHECK(writes3d>before);
    }
    uint32_t pixels[2][256]; uint16_t depth[2][256];
    for (unsigned side=0;side<2;side++) {
        CHECK(!render3d_readback(services[side],9,c[side],h[side][3],pixels[side],sizeof(pixels[side])));
        CHECK(!render3d_readback(services[side],9,c[side],h[side][1],depth[side],sizeof(depth[side])));
        if (expected && pixels[side][0]!=*expected) {
            fprintf(stderr,"fixed case=%u side=%u pixel=0 actual=%08x expected=%08x\n",case_id,side,pixels[side][0],*expected);
            CHECK(false); exit(1);
        }
    }
    for (unsigned i=0;i<256;i++) {
        /* Sentinel outside geometry is an exact coverage oracle. */
        CHECK((pixels[0][i]==q->clear_color)==(pixels[1][i]==q->clear_color));
        for (unsigned shift=0;shift<32;shift+=8) {
            int delta=(int)(pixels[0][i]>>shift&255)-(int)(pixels[1][i]>>shift&255);
            if (abs(delta)>2) {
                fprintf(stderr,"fixed case=%u pixel=%u channel=%u gpu=%08x scalar=%08x\n",case_id,i,shift,pixels[0][i],pixels[1][i]);
                CHECK(false); exit(1);
            }
        }
        CHECK(abs((int)depth[0][i]-(int)depth[1][i])<=1);
    }
    for (unsigned side=0;side<2;side++) CHECK(!render3d_destroy_context(services[side],9,c[side]));
    CHECK(!allocations3d && !scratch3d.ram_bytes && !reference3d.ram_bytes);
    CHECK(!locks && !drawing && !count3d);
    (void)host;
}
static void equivalent3d(const struct render3d_host *host,struct render3d_request *q,
    const struct render3d_texture_desc *tex,bool software,unsigned case_id)
{ equivalent_textures3d(host,q,tex,0,software,case_id,0); }
static struct render3d_request fixed_request3d(void)
{
    struct render3d_request q={.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY,
        .clear_color=0x60402010,.clear_depth=32768,.count=3};
    q.vertices[0]=(struct render3d_vertex){0,0,16384,65536,0x80604020,-16384,-16384};
    q.vertices[1]=(struct render3d_vertex){16<<16,0,16384,65536,0x80a08060,49152,-16384};
    q.vertices[2]=(struct render3d_vertex){0,16<<16,16384,65536,0x80e0c0a0,-16384,49152};
    q.flat_color[0]=0x80604020;
    for (unsigned i=0;i<3;i++) { q.uv1[i][0]=q.vertices[i].v; q.uv1[i][1]=q.vertices[i].u; }
    return q;
}
static void replace_selectors3d(const struct render3d_host *host)
{
    static uint8_t first_image[sizeof(images3d[0])];
    const unsigned second_formats[]={R3D_RGB565,R3D_L8,R3D_A8};
    unsigned cases=0;
    for (unsigned first=0;first<6;first++) for (unsigned second=0;second<3;second++)
    for (unsigned combine=0;combine<4;combine++) for (unsigned fragment=0;fragment<3;fragment++) {
        if (first==R3D_A8 && combine==R3D_DECAL) continue; /* separately tested software state */
        struct render3d_texture_desc a,b;
        texture_fixture3d(&a,first,16,1);
        memcpy(first_image,a.image[0].pixels,a.image[0].bytes); a.image[0].pixels=first_image;
        texture_fixture3d(&b,second_formats[second],32,1);
        struct render3d_request q=fixed_request3d(); q.flags|=R3D_TEXTURED|R3D_TEXTURE1;
        q.state.sampler[0].combine=combine; q.state.sampler[1].combine=R3D_REPLACE;
        if (fragment==1) { q.state.blend=1; q.state.source=R3D_SRC_ALPHA; q.state.destination=R3D_INV_SRC_ALPHA; }
        if (fragment==2) { q.state.alpha_test=1; q.state.alpha_func=R3D_LESS; q.state.alpha_ref=112; }
        equivalent_textures3d(host,&q,&a,&b,false,10000+cases++,0);
    }
    CHECK(cases==207);
    printf("Savage stage-1 REPLACE: %u distinct-texture mixed-format scalar/packet cases, alpha rejection/depth writes/blending PASS\n",cases);
}
static void signed_coordinates3d(const struct render3d_host *host)
{
    uint32_t pixels[64];
    for (unsigned i=0;i<64;i++) pixels[i]=i==0 ? 0xffff0000u : i==63 ? 0xff0000ffu : 0xff00ff00u;
    unsigned cases=0;
    for (unsigned stage=0;stage<2;stage++) for (unsigned axis=0;axis<2;axis++)
    for (unsigned sign=0;sign<2;sign++) for (unsigned varying=0;varying<2;varying++) {
        struct render3d_texture_desc t={.format=R3D_ARGB8888,.width=axis ? 1 : 64,.height=axis ? 64 : 1,.levels=1};
        t.image[0]=(struct render3d_texture_image){pixels,sizeof(pixels),t.width*4};
        struct render3d_request q=fixed_request3d(); q.flags|=R3D_TEXTURED|(stage ? R3D_TEXTURE1 : 0);
        for (unsigned i=0;i<3;i++) {
            q.vertices[i].rhw=256+(varying ? i*128 : 0);
            q.vertices[i].u=q.vertices[i].v=q.uv1[i][0]=q.uv1[i][1]=0;
            int32_t coord=sign ? 128 : -128;
            if (stage) q.uv1[i][axis]=coord;
            else if (axis) q.vertices[i].v=coord;
            else q.vertices[i].u=coord;
        }
        for (unsigned s=0;s<2;s++) q.state.sampler[s]=(struct render3d_sampler){R3D_REPLACE,R3D_NEAREST,1,1};
        uint32_t expected=sign ? 0xffff0000u : 0xff0000ffu;
        equivalent_textures3d(host,&q,&t,0,false,11000+cases++,&expected);
    }
    CHECK(cases==16);
    printf("Savage signed perspective UV: %u contrasting-texel cases, both stages/axes, reciprocal W 1/256 and varying, signed repeat PASS\n",cases);
}
static void fixed_function3d(const struct render3d_host *host)
{
    CHECK(!render3d_close(&scratch3d));
    render3d_init(&scratch3d,host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    render3d_init(&reference3d,host,42,0,0,(struct render3d_extent){0});
    replace_selectors3d(host);
    signed_coordinates3d(host);
    unsigned cases=0;
    for (unsigned func=0;func<8;func++) for (unsigned z=0;z<3;z++) for (unsigned write=0;write<2;write++) {
        struct render3d_request q=fixed_request3d(); q.state.depth_func=func;
        if (!write) q.flags&=~R3D_DEPTH_WRITE;
        for (unsigned i=0;i<3;i++) q.vertices[i].z=z==0 ? 16384 : z==1 ? 32768 : 49152;
        equivalent3d(host,&q,0,false,cases++);
    }
    for (unsigned func=0;func<8;func++) for (unsigned a=0;a<3;a++) {
        struct render3d_request q=fixed_request3d(); q.state.alpha_test=1; q.state.alpha_func=func; q.state.alpha_ref=128;
        for (unsigned i=0;i<3;i++) q.vertices[i].color=(q.vertices[i].color&0xffffffu)|((a==0 ? 0u : a==1 ? 128u : 255u)<<24);
        equivalent3d(host,&q,0,false,cases++);
    }
    for (unsigned src=0;src<4;src++) for (unsigned dst=0;dst<4;dst++) for (unsigned a=0;a<3;a++) {
        struct render3d_request q=fixed_request3d(); q.state.blend=1; q.state.source=src; q.state.destination=dst;
        for (unsigned i=0;i<3;i++) q.vertices[i].color=(q.vertices[i].color&0xffffffu)|((a==0 ? 0u : a==1 ? 128u : 255u)<<24);
        equivalent3d(host,&q,0,false,cases++);
    }
    for (unsigned format=0;format<6;format++) for (unsigned combine=0;combine<4;combine++)
    for (unsigned filter=0;filter<5;filter++) for (unsigned wrap=0;wrap<4;wrap++)
    for (unsigned stage=0;stage<2;stage++) for (unsigned flat=0;flat<2;flat++) {
        struct render3d_texture_desc tex; texture_fixture3d(&tex,format,64,7);
        struct render3d_request q=fixed_request3d(); q.flags|=R3D_TEXTURED|(stage ? R3D_TEXTURE1 : 0); q.state.flat=flat;
        for (unsigned s=0;s<2;s++) q.state.sampler[s]=(struct render3d_sampler){combine,filter,wrap&1,wrap>>1};
        equivalent3d(host,&q,&tex,format==R3D_A8 && combine==R3D_DECAL,cases++);
    }
    for (unsigned power=0;power<7;power++) for (unsigned format=0;format<6;format++)
    for (unsigned filter=0;filter<5;filter++) for (unsigned stage=0;stage<2;stage++) {
        struct render3d_texture_desc tex; texture_fixture3d(&tex,format,1u<<power,power+1);
        struct render3d_request q=fixed_request3d(); q.flags|=R3D_TEXTURED|(stage ? R3D_TEXTURE1 : 0);
        for (unsigned s=0;s<2;s++) q.state.sampler[s]=(struct render3d_sampler){R3D_MODULATE,filter,1,1};
        equivalent3d(host,&q,&tex,false,cases++);
    }
    for (unsigned filter=0;filter<5;filter++) for (unsigned stage=0;stage<2;stage++) {
        struct render3d_texture_desc tex; texture_fixture3d(&tex,R3D_ARGB8888,64,7);
        struct render3d_request q=fixed_request3d(); q.flags|=R3D_TEXTURED|(stage ? R3D_TEXTURE1 : 0);
        q.vertices[1].rhw=4*65536; q.vertices[2].rhw=2*65536;
        for (unsigned s=0;s<2;s++) q.state.sampler[s]=(struct render3d_sampler){R3D_MODULATE,filter,1,0};
        equivalent3d(host,&q,&tex,false,cases++);
    }
    for (unsigned i=0;i<64;i++) {
        struct render3d_texture_desc tex; texture_fixture3d(&tex,R3D_ARGB4444,64,7);
        struct render3d_request q=fixed_request3d(); q.flags|=R3D_TEXTURED|R3D_TEXTURE1;
        q.state.alpha_test=q.state.blend=1; q.state.alpha_ref=96;
        q.state.depth_func=i%8; q.state.alpha_func=(i/8)%8;
        q.state.source=i%4; q.state.destination=(i/4)%4; q.state.flat=i%2;
        q.state.sampler[0]=(struct render3d_sampler){R3D_ADD,R3D_BILINEAR_MIP,1,0};
        q.state.sampler[1]=(struct render3d_sampler){R3D_DECAL,R3D_TRILINEAR,0,1};
        equivalent3d(host,&q,&tex,false,cases++);
    }
    CHECK(cases==2534);
    CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d));
    printf("Savage fixed-function: %u new scalar/packet cases (8 depth/alpha functions, 16 blend pairs, 6 formats, 5 filters, wrap/clamp, 2 stages, flat/Gouraud, whole-draw fallback) PASS\n",cases);
}
static void texture_security3d(const struct render3d_host *host)
{
    render3d_init(&scratch3d,host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    uint32_t c,target,depth,copy,tex;
    CHECK(!render3d_create_context(&scratch3d,9,R3D_AUTO,&c));
    CHECK(!render3d_create_resource(&scratch3d,9,c,R3D_COLOR,16,16,0,0,&target));
    CHECK(!render3d_create_resource(&scratch3d,9,c,R3D_D16,16,16,0,0,&depth));
    CHECK(!render3d_create_resource(&scratch3d,9,c,R3D_LINEAR_COLOR,16,16,0,0,&copy));
    struct render3d_texture_desc t; texture_fixture3d(&t,R3D_ARGB8888,64,7);
    CHECK(!render3d_create_texture(&scratch3d,9,c,&t,&tex));
    struct render3d_request good=fixed_request3d(); good.flags|=R3D_TEXTURED|R3D_TEXTURE1;
    good.target=target; good.depth=depth; good.copy_target=copy; good.texture=good.texture1=tex;
    struct render3d_fence f;
    CHECK(!render3d_submit(&scratch3d,9,c,&good,sizeof(good),&f)); CHECK(render3d_step(&scratch3d)==1);
    unsigned before=writes3d,mem=memory3d;
    for (unsigned mutation=0;mutation<17;mutation++) {
        struct render3d_request q=good;
        switch (mutation) {
        case 0: q.state.depth_func=8; break; case 1: q.state.alpha_func=8; break;
        case 2: q.state.alpha_ref=256; break; case 3: q.state.alpha_test=2; break;
        case 4: q.state.source=4; break; case 5: q.state.destination=4; break;
        case 6: q.state.flat=2; break; case 7: q.state.blend=2; break;
        case 8: q.state.sampler[0].combine=4; break; case 9: q.state.sampler[1].filter=5; break;
        case 10: q.state.sampler[0].repeat_u=2; break; case 11: q.state.sampler[1].repeat_v=2; break;
        case 12: q.uv1[0][0]=INT32_MIN; break; case 13: q.vertices[0].u=INT32_MAX; break;
        case 14: q.flags&=~R3D_TEXTURED; break; case 15: q.clear_depth=65536; break;
        case 16: q.texture1=UINT32_MAX; break;
        }
        CHECK(render3d_submit(&scratch3d,9,c,&q,sizeof(q),&f)==-EINVAL);
        CHECK(writes3d==before && memory3d==mem && !scratch3d.queued);
    }
    CHECK(render3d_submit(&scratch3d,9,c,&good,sizeof(good)-1,&f)==-EINVAL);
    CHECK(render3d_submit(&scratch3d,9,c,&good,sizeof(good)+1,&f)==-EINVAL);
    struct render3d_resource *r[R3D_REFS]={&scratch3d.contexts[0].resources[0],&scratch3d.contexts[0].resources[1],
        &scratch3d.contexts[0].resources[3],&scratch3d.contexts[0].resources[2],&scratch3d.contexts[0].resources[3]};
    CHECK(!savage3d_backend()->validate(&gpu3d,&good,r));
    for (unsigned mutation=0;mutation<14;mutation++) {
        struct render3d_resource saved=*r[2];
        switch (mutation) {
        case 0: r[2]->offset=gpu3d.heap.first-4096; break;
        case 1: r[2]->offset=gpu3d.heap.first+gpu3d.heap.bytes-4096; break;
        case 2: r[2]->offset++; break;
        case 3: r[2]->reserved=UINT32_MAX; break;
        case 4: r[2]->bytes=UINT32_MAX; break;
        case 5: r[2]->pitch=UINT32_MAX; break;
        case 6: r[2]->width=UINT32_MAX; break;
        case 7: r[2]->height=UINT32_MAX; break;
        case 8: r[2]->level_offset[1]=UINT32_MAX; break;
        case 9: r[2]->levels=12; break;
        case 10: r[2]->levels=0; break;
        case 11: r[2]->format=6; break;
        case 12: r[2]->shadow=0; break;
        case 13: r[2]->offset=r[0]->offset; break;
        }
        CHECK(savage3d_backend()->validate(&gpu3d,&good,r)==-EINVAL);
        CHECK(writes3d==before && memory3d==mem);
        *r[2]=saved;
    }
    for (unsigned mutation=0;mutation<9;mutation++) {
        struct render3d_texture_desc bad=t;
        switch (mutation) {
        case 0: bad.image[0].pitch=UINT32_MAX; break;
        case 1: bad.image[0].pitch=1; break;
        case 2: bad.image[0].bytes--; break;
        case 3: bad.image[6].pixels=0; break;
        case 4: bad.levels=8; break;
        case 5: bad.format=6; break;
        case 6: bad.width=UINT32_MAX; break;
        case 7: bad.image[0].pitch=R3D_RAM_LIMIT; bad.image[0].bytes=(size_t)R3D_RAM_LIMIT*64; break;
        case 8: bad.image[0].pixels=(void *)(UINTPTR_MAX-8); break;
        }
        uint32_t h; CHECK(render3d_create_texture(&scratch3d,9,c,&bad,&h)==-EINVAL);
        CHECK(writes3d==before && memory3d==mem);
    }
    /* Alternate hardware, unsupported A8 DECAL, then hardware on the SAME
     * target without clearing: canonical color/depth must survive both ways. */
    struct render3d_texture_desc alpha; texture_fixture3d(&alpha,R3D_A8,64,1);
    uint32_t ah; CHECK(!render3d_create_texture(&scratch3d,9,c,&alpha,&ah));
    good.flags&=~(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_TEXTURE1); good.texture1=0;
    good.state.depth_func=R3D_ALWAYS; good.texture=ah; good.state.sampler[0].combine=R3D_DECAL;
    CHECK(!render3d_submit(&scratch3d,9,c,&good,sizeof(good),&f));
    while (scratch3d.queued) CHECK(render3d_step(&scratch3d)==1);
    CHECK(writes3d==before);
    uint32_t expected[256]; CHECK(!render3d_readback(&scratch3d,9,c,copy,expected,sizeof(expected)));
    CHECK(expected[0]==0x80664626);
    good.texture=tex; good.state.sampler[0].combine=R3D_MODULATE;
    good.state.blend=1; good.state.source=R3D_SRC_ALPHA; good.state.destination=R3D_INV_SRC_ALPHA;
    CHECK(!render3d_submit(&scratch3d,9,c,&good,sizeof(good),&f)); CHECK(render3d_step(&scratch3d)==1);
    CHECK(writes3d>before && scratch3d.contexts[0].hardware);
    CHECK(!render3d_readback(&scratch3d,9,c,copy,expected,sizeof(expected)) && expected[0]==0x71553b21);
    /* Large canonical textures remain available without silently raising the
     * probe's 64x64 execution ceiling. Hardware-required refuses before MMIO. */
    static uint32_t large[128*128];
    for (unsigned i=0;i<128*128;i++) large[i]=UINT32_MAX;
    uint32_t lh; CHECK(!render3d_create_resource(&scratch3d,9,c,R3D_TEXTURE,128,128,large,sizeof(large),&lh));
    good.texture=lh; before=writes3d;
    scratch3d.contexts[0].policy=R3D_HARDWARE_REQUIRED;
    CHECK(render3d_submit(&scratch3d,9,c,&good,sizeof(good),&f)==-EOPNOTSUPP && writes3d==before);
    scratch3d.contexts[0].policy=R3D_AUTO;
    CHECK(!render3d_submit(&scratch3d,9,c,&good,sizeof(good),&f));
    while (scratch3d.queued) CHECK(render3d_step(&scratch3d)==1);
    CHECK(writes3d==before && scratch3d.contexts[0].hardware);
    CHECK(!render3d_readback(&scratch3d,9,c,copy,expected,sizeof(expected)) && expected[0]==0x795e4124);
    uint32_t tiled_copy; CHECK(!render3d_create_resource(&scratch3d,9,c,R3D_COLOR,16,16,0,0,&tiled_copy));
    good.texture=tex; good.copy_target=tiled_copy;
    CHECK(!render3d_submit(&scratch3d,9,c,&good,sizeof(good),&f));
    while (scratch3d.queued) CHECK(render3d_step(&scratch3d)==1);
    CHECK(writes3d==before);
    CHECK(!render3d_readback(&scratch3d,9,c,tiled_copy,expected,sizeof(expected)) && expected[0]==0x6b4f371f);
    CHECK(!render3d_close(&scratch3d) && !allocations3d);
    puts("Savage new state/extent/mip/size/overflow no-write rejection and hardware/scalar/hardware transitions PASS");
}
static void texture_lru3d(const struct render3d_host *host)
{
    render3d_init(&scratch3d,host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    uint32_t c,target,tex[40];
    CHECK(!render3d_create_context(&scratch3d,9,R3D_HARDWARE_REQUIRED,&c));
    CHECK(!render3d_create_resource(&scratch3d,9,c,R3D_COLOR,16,16,0,0,&target));
    struct render3d_texture_desc t; texture_fixture3d(&t,R3D_ARGB8888,64,1);
    for (unsigned i=0;i<40;i++) CHECK(!render3d_create_texture(&scratch3d,9,c,&t,&tex[i]));
    struct render3d_request q=fixed_request3d(); q.flags=R3D_CLEAR_COLOR|R3D_TEXTURED; q.target=target;
    for (unsigned draw=0;draw<42;draw++) {
        unsigned i=draw<40 ? draw : draw==40 ? 39 : 0;
        q.texture=tex[i]; struct render3d_fence f;
        CHECK(!render3d_submit(&scratch3d,9,c,&q,sizeof(q),&f));
        struct render3d_resource *r=&scratch3d.contexts[0].resources[i+1];
        CHECK(r->references==1 && r->offset>=scratch3d.texture_first);
        unsigned before=writes3d;
        CHECK(render3d_submit(&scratch3d,9,c,&q,sizeof(q),&f)==-EAGAIN && writes3d==before);
        CHECK(render3d_step(&scratch3d)==1 && !r->references && r->resident);
        CHECK(r->offset+r->reserved<=scratch3d.heap_end);
        if (draw==39) {
            CHECK(!scratch3d.contexts[0].resources[1].resident && !scratch3d.contexts[0].resources[1].offset);
            CHECK(scratch3d.contexts[0].resources[40].resident);
        }
    }
    /* The first texture was reuploaded after eviction; its shadow is intact. */
    CHECK(((uint32_t *)scratch3d.contexts[0].resources[1].shadow)[0]==0x60203040);
    q.texture=tex[0]; struct render3d_fence f;
    CHECK(!render3d_submit(&scratch3d,9,c,&q,sizeof(q),&f));
    uint32_t offset=scratch3d.contexts[0].resources[1].offset;
    CHECK(!render3d_destroy_resource(&scratch3d,9,c,tex[0]));
    CHECK(scratch3d.contexts[0].resources[1].live && scratch3d.contexts[0].resources[1].offset==offset);
    CHECK(render3d_step(&scratch3d)==1 && !scratch3d.contexts[0].resources[1].live);
    CHECK(!render3d_close(&scratch3d) && !allocations3d);
    puts("Savage separate 256 KiB pool, immutable shadows, LRU eviction/reupload, queued pin and pending-free fences PASS");
}
static void mip_addresses3d(void)
{
    uint32_t old_td=m.reg[0x485a8/4],old_addr=m.reg[0x48594/4];
    unsigned fixtures=0;
    for (unsigned fmt=0;fmt<3;fmt++) for (unsigned wp=0;wp<=10;wp++) for (unsigned hp=0;hp<=10;hp++) {
        unsigned format=fmt==0 ? R3D_ARGB8888 : fmt==1 ? R3D_RGB565 : R3D_ARGB4444;
        struct render3d_resource r={.kind=R3D_TEXTURE,.format=format,.width=1u<<wp,.height=1u<<hp,
            .pitch=4u<<wp,.levels=(wp>hp ? wp : hp)+1};
        unsigned w=r.width,h=r.height;
        for (unsigned l=0;l<r.levels;l++) {
            r.level_offset[l]=r.bytes; r.bytes+=w*h*4;
            w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
        }
        uint32_t storage=savage3d_texture_bytes(&r);
        CHECK(storage && !(storage&4095u));
        m.reg[0x485a8/4]=wp|(hp<<4)|((fmt==0 ? 3u : fmt==1 ? 6u : 5u)<<8);
        m.reg[0x48594/4]=gpu3d.heap.first|2;
        w=r.width; h=r.height;
        for (unsigned l=0;l<r.levels;l++) {
            unsigned xs[]={0,1,3,4,7,8,15,16,31,32,63,64,w-1};
            unsigned ys[]={0,1,3,4,7,8,15,16,31,32,63,64,h-1};
            for (unsigned xi=0;xi<13;xi++) for (unsigned yi=0;yi<13;yi++) if (xs[xi]<w && ys[yi]<h) {
                uint32_t off=UINT32_MAX;
                CHECK(!savage3d_mip_offset(&r,l,xs[xi],ys[yi],&off));
                CHECK(off==texture_address3d(0,l,xs[xi],ys[yi])-gpu3d.heap.first);
                CHECK(off+(fmt ? 2 : 4)<=storage); fixtures++;
            }
            w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
        }
    }
    /* Literal 8x8->4x4->2x2->1x1 ARGB tail: the three small
     * levels share a 256-byte region, at relative offsets 0,16,144. */
    struct render3d_resource r={.kind=R3D_TEXTURE,.width=8,.height=8,.pitch=32,.bytes=340,
        .levels=4,.level_offset={0,256,320,336}};
    uint32_t off;
    CHECK(!savage3d_mip_offset(&r,1,0,0,&off) && off==256);
    CHECK(!savage3d_mip_offset(&r,2,0,0,&off) && off==272);
    CHECK(!savage3d_mip_offset(&r,3,0,0,&off) && off==400);
    r.level_offset[3]++; CHECK(!savage3d_texture_bytes(&r));
    m.reg[0x485a8/4]=old_td; m.reg[0x48594/4]=old_addr;
    printf("Savage mip layout: %u independent tile/tail boundary addresses, 363 format/dimension chains through 1024, literal shared-tail offsets PASS\n",fixtures);
}
