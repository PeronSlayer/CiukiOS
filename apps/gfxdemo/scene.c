/* Integer software rasterizer. Independent implementation; research and
 * numeric bounds in docs/design/graphics-demo.md. SPDX-License-Identifier: MIT */
#include "gfxdemo.h"
#include "../accessories/acc.h"
#include <limits.h>
#include <string.h>
static int clamp(int n,int lo,int hi) { return n<lo ? lo : n>hi ? hi : n; }
static int64_t edge(struct gfx_vertex a,struct gfx_vertex b,int x,int y)
{ return (int64_t)(b.x-a.x)*(y-a.y)-(int64_t)(b.y-a.y)*(x-a.x); }
static int top_left(struct gfx_vertex a,struct gfx_vertex b)
{ return b.y<a.y || (b.y==a.y && b.x>a.x); }
/* Exact quotient/remainder interpolation. Only span setup divides by the
 * triangle area; walking fragments uses adds and one carry per attribute. */
struct interpolant { int64_t value,step; uint32_t remainder,delta; };
static int64_t floor_div(int64_t n,int64_t d)
{ int64_t q=n/d; return q-(n%d<0); }
static struct interpolant interpolate(int64_t n,int64_t dx,int64_t area)
{
    int64_t value=floor_div(n,area),step=floor_div(dx,area);
    return (struct interpolant){value,step,(uint32_t)(n-value*area),(uint32_t)(dx-step*area)};
}
static void increment(struct interpolant *p,uint32_t area)
{
    p->value+=p->step; p->remainder+=p->delta;
    if (p->remainder>=area) { p->remainder-=area; p->value++; }
}
void gfx_triangle(struct gfx_scene *s,struct gfx_vertex a,struct gfx_vertex b,struct gfx_vertex c,
                  const uint32_t *portrait,int gouraud,unsigned flat)
{
    struct gfx_vertex vertices[3]={a,b,c};
    for (unsigned i=0;i<3;i++) {
        struct gfx_vertex v=vertices[i];
        if (v.x < -16384 || v.x>16384 || v.y < -16384 || v.y>16384 ||
            v.inv<=0 || v.inv>65536 || v.u<0 || v.u>255*v.inv || v.v<0 || v.v>255*v.inv ||
            v.light<0 || v.light>255) return;
    }
    unsigned width=s->width ? s->width : GFX_W,height=s->height ? s->height : GFX_H;
    if (width>GFX_MAX_W || height>GFX_MAX_H) return;
    int64_t area=edge(a,b,c.x,c.y);
    if (!area) return;
    if (area<0) { struct gfx_vertex t=b;b=c;c=t;area=-area; }
    int xmin=a.x<b.x ? a.x : b.x, xmax=a.x>b.x ? a.x : b.x;
    int ymin=a.y<b.y ? a.y : b.y, ymax=a.y>b.y ? a.y : b.y;
    if (c.x<xmin) xmin=c.x;
    if (c.x>xmax) xmax=c.x;
    if (c.y<ymin) ymin=c.y;
    if (c.y>ymax) ymax=c.y;
    xmin=clamp(xmin/16,0,(int)width-1); xmax=clamp(xmax/16,0,(int)width-1);
    ymin=clamp(ymin/16,0,(int)height-1); ymax=clamp(ymax/16,0,(int)height-1);
    int64_t dx[3]={(int64_t)(b.y-c.y)*16,(int64_t)(c.y-a.y)*16,(int64_t)(a.y-b.y)*16};
    int64_t dy[3]={(int64_t)(c.x-b.x)*16,(int64_t)(a.x-c.x)*16,(int64_t)(b.x-a.x)*16};
    int64_t edges[3]={edge(b,c,xmin*16+8,ymin*16+8),edge(c,a,xmin*16+8,ymin*16+8),edge(a,b,xmin*16+8,ymin*16+8)};
    int minimum[3]={!top_left(b,c),!top_left(c,a),!top_left(a,b)};
    int attrs[4][3]={{a.inv,b.inv,c.inv},{a.u,b.u,c.u},{a.v,b.v,c.v},{a.light,b.light,c.light}};
    unsigned attributes=gouraud ? 4 : 3;
    int64_t steps[4]={0};
    for (unsigned k=0;k<attributes;k++) for (unsigned j=0;j<3;j++) steps[k]+=dx[j]*attrs[k][j];
    for (int y=ymin;y<=ymax;y++) {
        int64_t left=0,right=xmax-xmin;
        for (unsigned j=0;j<3;j++) {
            if (dx[j]>0) {
                int64_t n=-floor_div(edges[j]-minimum[j],dx[j]); if (n>left) left=n;
            } else if (dx[j]<0) {
                int64_t n=floor_div(edges[j]-minimum[j],-dx[j]); if (n<right) right=n;
            } else if (edges[j]<minimum[j]) right=-1;
        }
        if (left<=right) {
            struct interpolant p[4];
            for (unsigned k=0;k<attributes;k++) {
                int64_t n=0;
                for (unsigned j=0;j<3;j++) n+=(edges[j]+left*dx[j])*attrs[k][j];
                p[k]=interpolate(n,steps[k],area);
            }
            size_t at=(size_t)y*width+(unsigned)(xmin+left);
            for (int64_t x=left;x<=right;x++,at++) {
                unsigned inv=(unsigned)p[0].value;
                if (inv && inv>s->depth[at]) {
                    unsigned u=(unsigned)p[1].value/inv,vv=(unsigned)p[2].value/inv;
                    unsigned light=gouraud ? (unsigned)p[3].value : flat>255 ? 255 : flat;
                    uint32_t colour=portrait ? portrait[(vv>255 ? 255 : vv)*256+(u>255 ? 255 : u)] : 0xe8d7b9;
                    uint32_t r=((colour>>16)&255)*light/255,g=((colour>>8)&255)*light/255,bl=(colour&255)*light/255;
                    s->pixels[at]=(r<<16)|(g<<8)|bl; s->depth[at]=inv;
                }
                for (unsigned k=0;k<attributes;k++) increment(&p[k],(uint32_t)area);
            }
        }
        for (unsigned j=0;j<3;j++) edges[j]+=dy[j];
    }
}
static void backdrop(struct gfx_scene *s)
{
    unsigned width=s->width ? s->width : GFX_W,height=s->height ? s->height : GFX_H;
    memset(s->depth,0,(size_t)width*height*4);
    uint16_t columns[GFX_MAX_W];unsigned count=0;
    for (unsigned x=0;x<width;x++) if ((x*GFX_W/width)%32==0) columns[count++]=(uint16_t)x;
    for (unsigned y=0;y<height;y++) {
        unsigned by=y*GFX_H/height,h=by/4;
        int horizontal=by>175 && (by-176)%16==0;
        uint32_t colour=horizontal ? 0x354d62 : ((10+h/3)<<16)|((20+h/2)<<8)|(34+h);
        uint32_t *row=s->pixels+(size_t)y*width,*dst=row;size_t words=width;
        __asm__ volatile("cld\n\trep stosl" : "+D"(dst), "+c"(words) : "a"(colour) : "memory", "cc");
        if (by>175 && !horizontal) for (unsigned k=0;k<count;k++) row[columns[k]]=0x354d62;
    }
}
static void rotate(int x,int y,int z,int angle,int *ox,int *oy,int *oz)
{
    int phase=angle/1024,fraction=angle%1024;
    int sa=(acc_sin60(phase)*(1024-fraction)+acc_sin60(phase+1)*fraction)/1024;
    int ca=(acc_cos60(phase)*(1024-fraction)+acc_cos60(phase+1)*fraction)/1024;
    int sb=acc_sin60(7),cb=acc_cos60(7);
    int xx=(x*ca+z*sa)/1024,zz=(z*ca-x*sa)/1024;
    *ox=xx; *oy=(y*cb-zz*sb)/1024; *oz=(y*sb+zz*cb)/1024;
}
void gfx_cube(struct gfx_scene *s,uint64_t ns,const uint32_t *portrait,int gouraud)
{
    static const signed char points[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    static const unsigned char faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{4,5,1,0},{3,2,6,7}};
    static const signed char normals[6][3]={{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
    unsigned width=s->width ? s->width : GFX_W,height=s->height ? s->height : GFX_H;
    struct gfx_vertex vertices[8];
    int angle=(int)((ns%6000000000ull)*61440/6000000000ull);
    backdrop(s);
    for (unsigned i=0;i<8;i++) {
        int x,y,z;rotate(points[i][0]*1024,points[i][1]*1024,points[i][2]*1024,angle,&x,&y,&z);
        int camera_z=z+5120;
        vertices[i]=(struct gfx_vertex){(int)width*8+x*(int)(220*width/GFX_W)*16/camera_z,(int)height*8+y*(int)(220*height/GFX_H)*16/camera_z,
            (1<<26)/camera_z,0,0,clamp(180+(x-y-z)/32,64,255)};
    }
    for (unsigned f=0;f<6;f++) {
        int nx,ny,nz;rotate(normals[f][0]*1024,normals[f][1]*1024,normals[f][2]*1024,angle,&nx,&ny,&nz);
        unsigned light=(unsigned)clamp(180+(nx-ny-nz)/16,64,255);
        struct gfx_vertex v[4];
        for (unsigned j=0;j<4;j++) {
            v[j]=vertices[faces[f][j]];
            v[j].u=(j==1 || j==2 ? 255 : 0)*v[j].inv;
            v[j].v=(j>=2 ? 255 : 0)*v[j].inv;
        }
        gfx_triangle(s,v[0],v[1],v[2],portrait,gouraud,light);
        gfx_triangle(s,v[0],v[2],v[3],portrait,gouraud,light);
    }
}
static unsigned bounce(uint64_t ns,unsigned speed,unsigned offset,unsigned span)
{
    uint64_t p=((ns/1000000ull)*speed/1000+offset)%(span*2);
    return (unsigned)(p>span ? span*2-p : p);
}
void gfx_sprites(struct gfx_scene *s,uint64_t ns,const uint32_t *portrait)
{
    unsigned width=s->width ? s->width : GFX_W,height=s->height ? s->height : GFX_H;
    backdrop(s);
    for (unsigned n=0;n<16;n++) {
        unsigned size=(20+n%4*4)*width/GFX_W; if (!size) size=1;
        if (size>=width || size>=height) continue;
        unsigned x=bounce(ns,35+n*7,n*37,width-size),y=bounce(ns,27+n*5,n*23,height-size);
        for (unsigned yy=0;yy<size;yy++) for (unsigned xx=0;xx<size;xx++) {
            uint32_t c=portrait ? portrait[(yy*256/size)*256+xx*256/size] : 0xe8d7b9;
            s->pixels[(y+yy)*width+x+xx]=c;
        }
    }
    /* A clock-driven pacing marker crosses the bottom edge once per second. */
    unsigned marker=(unsigned)((ns%1000000000ull)*width/1000000000ull);
    for (unsigned y=height>3 ? height-3 : 0;y<height;y++) s->pixels[y*width+marker]=0xe8d7b9;
}
