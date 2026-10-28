/* Qualification rows must equal a complete raster, including resumed columns,
 * blending/depth dependencies and the final partial tile. GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/kernel/drivers/video/render3d.h"
static uint64_t tick;
static uint64_t now(void *ctx) { (void)ctx; tick+=500; return tick; }
static uint32_t color[2][128*65],copy[2][128*65];
static uint16_t depth[2][128*65];
int main(void)
{
    uint32_t texture[]={0xff102030,0xff90a0b0,0xff507090,0xffe0d0c0};
    struct render3d_resource resources[2][R3D_REFS]={0};
    struct render3d_resource *r[2][R3D_REFS];
    for (unsigned side=0;side<2;side++) {
        for (unsigned i=0;i<R3D_REFS;i++) {
            r[side][i]=&resources[side][i];
            *r[side][i]=(struct render3d_resource){.width=128,.height=65,.pitch=512,
                .bytes=128*65*4,.kind=R3D_COLOR,.shadow=i==3 ? copy[side] : color[side]};
        }
        resources[side][1].shadow=depth[side]; resources[side][1].pitch=256;
        resources[side][1].kind=R3D_D16;
        for (unsigned i=2;i<=4;i+=2) resources[side][i]=(struct render3d_resource){
            .width=2,.height=2,.pitch=8,.kind=R3D_TEXTURE,.format=R3D_ARGB8888,
            .levels=1,.shadow=texture,.bytes=sizeof(texture)};
    }
    memset(color,0x5a,sizeof(color)); memset(copy,0x5a,sizeof(copy)); memset(depth,0x5a,sizeof(depth));
    struct render3d_host host={.now_us=now};
    unsigned resumes=0;
    for (unsigned phase=0;phase<4;phase++) {
        struct render3d_request q={.flags=R3D_TEXTURED|R3D_TEXTURE1|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY,
            .clear_color=0xff102030,.clear_depth=65535,.count=6};
        if (!phase) q.flags|=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH;
        q.state.sampler[0].filter=q.state.sampler[1].filter=R3D_BILINEAR;
        q.state.blend=phase>=2; q.state.source=R3D_SRC_ALPHA; q.state.destination=R3D_INV_SRC_ALPHA;
        q.state.alpha_test=phase==3; q.state.alpha_func=R3D_GEQUAL; q.state.alpha_ref=128;
        static const unsigned corners[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
        for (unsigned i=0;i<6;i++) {
            unsigned x=corners[i][0],y=corners[i][1];
            q.vertices[i]=(struct render3d_vertex){x*128<<16,y*65<<16,phase==1 ? 60000 : 32000-phase*1000,
                x ? 65536 : 32768,0x80ffffff,x*65536,y*65536};
            q.uv1[i][0]=y*65536; q.uv1[i][1]=x*65536;
        }
        struct render3d_job full={0},sampled={0};
        while (!render3d_scalar_slice(&q,r[0],&full,&host)) {}
        while (!render3d_scalar_sample_slice(&q,r[1],&sampled,&host)) if (sampled.column) resumes++;
        for (unsigned y=0;y<65;y++) for (unsigned x=0;x<128;x++) {
            unsigned at=y*128+x;
            if (!(y%16) || y==64) {
                assert(color[0][at]==color[1][at] && copy[0][at]==copy[1][at] && depth[0][at]==depth[1][at]);
            } else assert(color[1][at]==0x5a5a5a5a && copy[1][at]==0x5a5a5a5a && depth[1][at]==0x5a5a);
        }
    }
    assert(resumes);
    printf("sampled scalar: four ordered phases, exact tile/final rows, %u column resumes, untouched other rows PASS\n",resumes);
    return 0;
}
