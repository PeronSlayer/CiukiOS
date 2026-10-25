/* Scalar minimum: top-left coverage, normalized D16 LESS, one texture stage.
 * All intermediates bounded by render3d.c admission. No floating point/SIMD.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "render3d.h"
static int64_t edge(const struct render3d_vertex *a, const struct render3d_vertex *b, int64_t x, int64_t y)
{ return ((int64_t)b->x-a->x)*(y-a->y)-((int64_t)b->y-a->y)*(x-a->x); }
static bool top_left(const struct render3d_vertex *a, const struct render3d_vertex *b)
{ return b->y<a->y || (b->y==a->y && b->x>a->x); }
/* floor(a/b * 65536) without overflowing the 52-bit edge product. */
static uint32_t fraction(uint64_t a, uint64_t b)
{
    if (a==b) return 65536;
    uint32_t q=0;
    for (unsigned i=0;i<16;i++) { a*=2; q<<=1; if (a>=b) { a-=b; q++; } }
    return q;
}
static uint32_t interpolate(uint32_t a, uint32_t b, uint32_t c, const uint32_t w[3])
{ return (uint32_t)(((uint64_t)a*w[0]+(uint64_t)b*w[1]+(uint64_t)c*w[2]+32768)>>16); }
static uint32_t color(const struct render3d_vertex v[3], const uint32_t w[3], uint32_t texture)
{
    uint32_t result=0;
    for (unsigned shift=0;shift<32;shift+=8) {
        uint32_t c=interpolate((v[0].color>>shift)&255,(v[1].color>>shift)&255,(v[2].color>>shift)&255,w);
        c=(c*((texture>>shift)&255)+127)/255;
        result|=c<<shift;
    }
    return result;
}
static void triangle(const struct render3d_request *q, struct render3d_resource *const r[4],
                     struct render3d_job *j, const struct render3d_host *host, uint64_t start)
{
    struct render3d_vertex v[3]; memcpy(v,&q->vertices[j->triangle*3],sizeof(v));
    int64_t area=edge(&v[0],&v[1],v[2].x,v[2].y);
    if (!area) { j->triangle++; j->row=0; return; }
    if (area<0) { struct render3d_vertex tmp=v[1]; v[1]=v[2]; v[2]=tmp; area=-area; }
    int32_t minx=v[0].x,maxx=v[0].x,miny=v[0].y,maxy=v[0].y;
    for (unsigned i=1;i<3;i++) {
        if (v[i].x<minx) minx=v[i].x;
        if (v[i].x>maxx) maxx=v[i].x;
        if (v[i].y<miny) miny=v[i].y;
        if (v[i].y>maxy) maxy=v[i].y;
    }
    unsigned first=(unsigned)miny>>16, end=(unsigned)(maxy+65535)>>16;
    unsigned row=j->row ? j->row : first, stop=row+4;
    if (stop>end) stop=end;
    for (unsigned y=row;y<stop;y++) {
      if (y>row && host && host->now_us(host->ctx)-start>=1000) { stop=y; break; }
      for (unsigned x=(unsigned)minx>>16;x<(unsigned)(maxx+65535)>>16;x++) {
        int64_t px=((int64_t)x<<16)+32768, py=((int64_t)y<<16)+32768;
        int64_t a=edge(&v[1],&v[2],px,py), b=edge(&v[2],&v[0],px,py), c=edge(&v[0],&v[1],px,py);
        if (a<0 || b<0 || c<0 || (!a && !top_left(&v[1],&v[2])) ||
            (!b && !top_left(&v[2],&v[0])) || (!c && !top_left(&v[0],&v[1]))) continue;
        uint32_t w[3]={fraction((uint64_t)a,(uint64_t)area),fraction((uint64_t)b,(uint64_t)area),0};
        w[2]=65536-w[0]-w[1];
        uint32_t z=interpolate(v[0].z,v[1].z,v[2].z,w);
        uint16_t *depth=r[1] ? (uint16_t *)r[1]->shadow+y*r[1]->width+x : 0;
        if ((q->flags&R3D_DEPTH_TEST) && z>=*depth) continue;
        uint32_t texel=UINT32_MAX;
        if (q->flags&R3D_TEXTURED) {
            uint32_t rhw=interpolate(v[0].rhw,v[1].rhw,v[2].rhw,w), uv[2];
            for (unsigned t=0;t<2;t++) {
                uint32_t a0=(uint32_t)(((uint64_t)(t ? v[0].v : v[0].u)*v[0].rhw)>>16);
                uint32_t a1=(uint32_t)(((uint64_t)(t ? v[1].v : v[1].u)*v[1].rhw)>>16);
                uint32_t a2=(uint32_t)(((uint64_t)(t ? v[2].v : v[2].u)*v[2].rhw)>>16);
                uv[t]=(uint32_t)(((uint64_t)interpolate(a0,a1,a2,w)<<16)/rhw);
                if (uv[t]>65536) uv[t]=65536;
            }
            unsigned tx=(unsigned)(((uint64_t)uv[0]*r[2]->width)>>16);
            unsigned ty=(unsigned)(((uint64_t)uv[1]*r[2]->height)>>16);
            if (tx==r[2]->width) tx--;
            if (ty==r[2]->height) ty--;
            texel=((uint32_t *)r[2]->shadow)[ty*r[2]->width+tx];
        }
        ((uint32_t *)r[0]->shadow)[y*r[0]->width+x]=color(v,w,texel);
        if (q->flags&R3D_DEPTH_WRITE) *depth=(uint16_t)z;
      }
    }
    j->row=stop;
    if (stop==end) { j->row=0; j->triangle++; }
}
int render3d_scalar_slice(const struct render3d_request *q, struct render3d_resource *const r[4],
                          struct render3d_job *j, const struct render3d_host *host)
{
    uint64_t start=host ? host->now_us(host->ctx) : 0;
    if (!j->phase) {
        if (q->flags&(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH)) {
            unsigned end=j->row+16; if (end>r[0]->height) end=r[0]->height;
            for (unsigned y=j->row;y<end;y++) {
              if (y>j->row && host && host->now_us(host->ctx)-start>=1000) { end=y; break; }
              for (unsigned x=0;x<r[0]->width;x++) {
                if (q->flags&R3D_CLEAR_COLOR) ((uint32_t *)r[0]->shadow)[y*r[0]->width+x]=q->clear_color;
                if (q->flags&R3D_CLEAR_DEPTH) ((uint16_t *)r[1]->shadow)[y*r[1]->width+x]=(uint16_t)q->clear_depth;
              }
            }
            j->row=end; if (end<r[0]->height) return 0;
        }
        j->row=0; j->phase=1;
        if ((q->flags&(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH)) && host && host->now_us(host->ctx)-start>=1000) return 0;
    }
    if (j->phase==1) {
        if (j->triangle<q->count/3) { triangle(q,r,j,host,start); return 0; }
        j->row=0; j->phase=2;
    }
    if (q->flags&R3D_COPY) {
        unsigned end=j->row+16; if (end>r[0]->height) end=r[0]->height;
        for (unsigned y=j->row;y<end;y++) {
            if (y>j->row && host && host->now_us(host->ctx)-start>=1000) { end=y; break; }
            memcpy((uint8_t *)r[3]->shadow+y*r[3]->pitch,
                   (uint8_t *)r[0]->shadow+y*r[0]->pitch,r[0]->pitch);
        }
        j->row=end; if (end<r[0]->height) return 0;
    }
    j->phase=3; return 1;
}
