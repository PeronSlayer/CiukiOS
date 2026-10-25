/* Independent wide-integer oracle for perspective recovery, including the
 * admitted signed neighbour weights used for mip selection.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../../src/kernel/drivers/video/render3d_scalar.c"

static unsigned cases;
static void recovery(const struct render3d_vertex v[3],const int32_t uv[3],const int64_t w[3])
{
    __int128 numerator=0,denominator=0;
    for (unsigned i=0;i<3;i++) {
        numerator+=(__int128)uv[i]*v[i].rhw*w[i];
        denominator+=(__int128)v[i].rhw*w[i];
    }
    __int128 expected=denominator>0 ? numerator/denominator : 32*65536;
    if (expected>32*65536) expected=32*65536;
    if (expected < -32*65536) expected=-32*65536;
    int32_t actual=coordinate(v,uv,w);
    if (actual!=(int32_t)expected) {
        fprintf(stderr,"perspective case=%u actual=%d expected=%d\n",cases,actual,(int32_t)expected);
        assert(actual==(int32_t)expected);
    }
    cases++;
}
static uint32_t random_word(void)
{
    static uint32_t state=0x574f4f46;
    state=state*1664525u+1013904223u;
    return state;
}
int main(void)
{
    struct render3d_vertex v[3]={{.rhw=256},{.rhw=256},{.rhw=256}};
    int32_t uv[3]={-128,-128,-128};
    int64_t w[3]={61440,2048,2048};
    recovery(v,uv,w);
    const int32_t coordinates[]={-1048576,-1048575,-65537,-129,-1,0,1,129,65537,1048575,1048576};
    const uint32_t reciprocals[]={256,257,8388863,8388864,16777215,16777216};
    const int64_t weights[][3]={{65536,0,0},{1,32767,32768},
        {1114112,1048576,-2097152},{-1048576,-1048576,2162688},
        {65538,-2,0},{65538,0,-2}};
    for (unsigned i=0;i<ARRAY_SIZE(coordinates);i++)
    for (unsigned j=0;j<ARRAY_SIZE(reciprocals);j++)
    for (unsigned k=0;k<ARRAY_SIZE(weights);k++) {
        for (unsigned n=0;n<3;n++) {
            uv[n]=coordinates[(i+n)%ARRAY_SIZE(coordinates)];
            v[n].rhw=reciprocals[(j+n)%ARRAY_SIZE(reciprocals)];
        }
        recovery(v,uv,weights[k]);
        /* Constant coordinates must survive even signed-weight cancellation
         * (including a denominator of two in the explicit case below). */
        for (unsigned n=0;n<3;n++) uv[n]=coordinates[i];
        recovery(v,uv,weights[k]);
    }
    v[0].rhw=256; v[1].rhw=8388863; v[2].rhw=256;
    for (unsigned i=0;i<ARRAY_SIZE(coordinates);i++) {
        for (unsigned n=0;n<3;n++) uv[n]=coordinates[i];
        recovery(v,uv,weights[4]);
    }
    v[1].rhw=8388864; recovery(v,uv,weights[4]); /* W = 0 */
    v[1].rhw=8388865; recovery(v,uv,weights[4]); /* W < 0 */
    for (unsigned n=0;n<3;n++) v[n].rhw=256;
    w[0]=-1; w[1]=65537; w[2]=0;
    uv[0]=65537; uv[1]=65536; uv[2]=0;
    recovery(v,uv,w); /* 65536 minus a fraction must truncate to 65535. */
    uv[0]=-65537; uv[1]=-65536;
    recovery(v,uv,w); /* Symmetric opposite-sign remainder. */
    for (unsigned trial=0;trial<20000;trial++) {
        uint32_t first=random_word()%65537,second=random_word()%(65537-first);
        int64_t dx=(int64_t)(random_word()%2097153)-1048576;
        int64_t dy=(int64_t)(random_word()%2097153)-1048576;
        w[0]=first+dx; w[1]=second+dy; w[2]=65536-w[0]-w[1];
        for (unsigned n=0;n<3;n++) {
            uv[n]=(int32_t)(random_word()%2097153)-1048576;
            v[n].rhw=256+random_word()%(16777216-255);
        }
        recovery(v,uv,w);
    }
    printf("Scalar perspective recovery: %u exact 128-bit oracle cases, signed UV, reciprocal-W bounds, signed mip neighbours/saturation PASS\n",cases);
    return 0;
}
