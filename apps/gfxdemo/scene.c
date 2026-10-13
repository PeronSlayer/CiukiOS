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
static int blend_value(int64_t a,int64_t b,int64_t c,int x,int y,int z,int64_t area)
{ return (int)((a*x+b*y+c*z)/area); }
void gfx_triangle(struct gfx_scene *s,struct gfx_vertex a,struct gfx_vertex b,struct gfx_vertex c,
                  const uint32_t *portrait,int gouraud,unsigned flat)
{
    /* Camera projects this cube inside +/-1024 pixels, Q4. Refuse unbounded
     * fixtures before subtraction/multiply; all interpolants fit int64_t. */
    struct gfx_vertex vertices[3]={a,b,c};
    for (unsigned i=0;i<3;i++) {
        struct gfx_vertex v=vertices[i];
        if (v.x < -16384 || v.x>16384 || v.y < -16384 || v.y>16384 ||
            v.inv<=0 || v.inv>65536 || v.u<0 || v.u>255*v.inv || v.v<0 || v.v>255*v.inv ||
            v.light<0 || v.light>255) return;
    }
    int64_t area=edge(a,b,c.x,c.y);
    if (!area) return;
    if (area<0) { struct gfx_vertex t=b;b=c;c=t;area=-area; }
    int xmin=a.x<b.x ? a.x : b.x, xmax=a.x>b.x ? a.x : b.x;
    int ymin=a.y<b.y ? a.y : b.y, ymax=a.y>b.y ? a.y : b.y;
    if (c.x<xmin) xmin=c.x;
    if (c.x>xmax) xmax=c.x;
    if (c.y<ymin) ymin=c.y;
    if (c.y>ymax) ymax=c.y;
    xmin=clamp(xmin/16,0,GFX_W-1); xmax=clamp(xmax/16,0,GFX_W-1);
    ymin=clamp(ymin/16,0,GFX_H-1); ymax=clamp(ymax/16,0,GFX_H-1);
    for (int y=ymin;y<=ymax;y++) for (int x=xmin;x<=xmax;x++) {
        int px=x*16+8,py=y*16+8;
        int64_t wa=edge(b,c,px,py),wb=edge(c,a,px,py),wc=edge(a,b,px,py);
        if (wa<0 || wb<0 || wc<0 || (!wa&&!top_left(b,c)) || (!wb&&!top_left(c,a)) || (!wc&&!top_left(a,b))) continue;
        unsigned inv=(unsigned)blend_value(wa,wb,wc,a.inv,b.inv,c.inv,area);
        size_t at=(size_t)y*GFX_W+(unsigned)x;
        if (!inv || inv<=s->depth[at]) continue; /* greater reciprocal depth is nearer */
        int u=blend_value(wa,wb,wc,a.u,b.u,c.u,area)/(int)inv;
        int v=blend_value(wa,wb,wc,a.v,b.v,c.v,area)/(int)inv;
        unsigned light=gouraud ? (unsigned)blend_value(wa,wb,wc,a.light,b.light,c.light,area) : flat>255 ? 255 : flat;
        uint32_t colour=portrait ? portrait[(unsigned)clamp(v,0,255)*256+(unsigned)clamp(u,0,255)] : 0xe8d7b9;
        uint32_t r=((colour>>16)&255)*light/255, g=((colour>>8)&255)*light/255, bl=(colour&255)*light/255;
        s->pixels[at]=(r<<16)|(g<<8)|bl; s->depth[at]=inv;
    }
}
static void backdrop(struct gfx_scene *s)
{
    memset(s->depth,0,sizeof(s->depth));
    for (int y=0;y<GFX_H;y++) for (int x=0;x<GFX_W;x++) {
        unsigned h=(unsigned)y/4;
        s->pixels[y*GFX_W+x]=((10+h/3)<<16)|((20+h/2)<<8)|(34+h);
        if (y>175 && (x%32==0 || (y-176)%16==0)) s->pixels[y*GFX_W+x]=0x354d62;
    }
}
static void rotate(int x,int y,int z,int angle,int *ox,int *oy,int *oz)
{
    int sa=acc_sin60(angle),ca=acc_cos60(angle),sb=acc_sin60(7),cb=acc_cos60(7);
    int xx=(x*ca+z*sa)/1024,zz=(z*ca-x*sa)/1024;
    *ox=xx; *oy=(y*cb-zz*sb)/1024; *oz=(y*sb+zz*cb)/1024;
}
void gfx_cube(struct gfx_scene *s,uint64_t ns,const uint32_t *portrait,int gouraud)
{
    static const signed char points[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    static const unsigned char faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{4,5,1,0},{3,2,6,7}};
    static const signed char normals[6][3]={{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
    struct gfx_vertex vertices[8];
    int angle=(int)((ns/100000000ull)%60);
    backdrop(s);
    for (unsigned i=0;i<8;i++) {
        int x,y,z;rotate(points[i][0]*1024,points[i][1]*1024,points[i][2]*1024,angle,&x,&y,&z);
        int camera_z=z+5120;
        vertices[i]=(struct gfx_vertex){GFX_W*8+x*220*16/camera_z,GFX_H*8+y*220*16/camera_z,
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
    backdrop(s);
    for (unsigned n=0;n<16;n++) {
        unsigned size=20+n%4*4;
        unsigned x=bounce(ns,35+n*7,n*37,GFX_W-size),y=bounce(ns,27+n*5,n*23,GFX_H-size);
        for (unsigned yy=0;yy<size;yy++) for (unsigned xx=0;xx<size;xx++) {
            uint32_t c=portrait ? portrait[(yy*256/size)*256+xx*256/size] : 0xe8d7b9;
            s->pixels[(y+yy)*GFX_W+x+xx]=c;
        }
    }
    /* A clock-driven pacing marker crosses the bottom edge once per second. */
    unsigned marker=(unsigned)((ns%1000000000ull)*GFX_W/1000000000ull);
    for (unsigned y=GFX_H-3;y<GFX_H;y++) s->pixels[y*GFX_W+marker]=0xe8d7b9;
}
