/* TinyGL scenes; exact approved Ciuki texture. SPDX-License-Identifier: MIT */
#include "gfxdemo.h"
#include <GL/gl.h>
#include <ciuki/gl.h>
#include <errno.h>
#include <stdlib.h>

static unsigned texture_generation;
static const uint32_t *texture_source;
static GLuint texture;

static int begin(struct gfx_scene *s,const uint32_t *portrait)
{
    unsigned w=s->width ? s->width : GFX_W,h=s->height ? s->height : GFX_H;
    s->error=0;
    if (w>GFX_MAX_W || h>GFX_MAX_H) { s->error=-EINVAL;return 0; }
    s->error=ciuki_gl_bind(s->pixels,w,h,w);
    if (s->error) return 0;
    if (texture_generation!=ciuki_gl_generation() || texture_source!=portrait || !texture) {
        unsigned char *rgb=malloc(256*256*3);
        if (!rgb) { s->error=-ENOMEM;return 0; }
        for (unsigned i=0;i<256*256;i++) {
            uint32_t p=portrait ? portrait[i] : 0xe8d7b9;
            rgb[i*3]=(unsigned char)(p>>16);rgb[i*3+1]=(unsigned char)(p>>8);rgb[i*3+2]=(unsigned char)p;
        }
        if (texture_generation!=ciuki_gl_generation()) texture=0;
        if (!texture) glGenTextures(1,&texture);
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,3,256,256,0,GL_RGB,GL_UNSIGNED_BYTE,rgb);
        free(rgb);texture_source=portrait;texture_generation=ciuki_gl_generation();
    }
    glBindTexture(GL_TEXTURE_2D,texture);
    glEnable(GL_TEXTURE_2D);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);glDisable(GL_COLOR_MATERIAL);glDisable(GL_NORMALIZE);
    glDepthMask(1);glEnable(GL_DEPTH_TEST);
    glClearColor(0.04f,0.08f,0.14f,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glColor3f(1,1,1);
    return 1;
}
static void finish(struct gfx_scene *s)
{
    ciuki_gl_finish();
    GLenum error=glGetError();if (error) s->error=(int)error;
}
void gfx_cube(struct gfx_scene *s,uint64_t ns,const uint32_t *portrait,int gouraud)
{
    static const GLfloat points[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    static const unsigned char faces[6][4]={{1,0,3,2},{4,5,6,7},{0,4,7,3},{5,1,2,6},{0,1,5,4},{7,6,2,3}};
    static const GLfloat normals[6][3]={{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
    static GLfloat light[4]={-1,2,3,0},diffuse[4]={0.85f,0.85f,0.85f,1};
    static GLfloat ambient[4]={0.25f,0.25f,0.25f,1},material[4]={1,1,1,1};
    if (!begin(s,portrait)) return;
    unsigned w=s->width ? s->width : GFX_W,h=s->height ? s->height : GFX_H;
    GLfloat aspect=(GLfloat)w/(GLfloat)h;
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glFrustum(-aspect,aspect,-1,1,2,20);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    glLightfv(GL_LIGHT0,GL_POSITION,light);glLightfv(GL_LIGHT0,GL_DIFFUSE,diffuse);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT,ambient);
    glMaterialfv(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE,material);
    glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);
    glShadeModel(gouraud ? GL_SMOOTH : GL_FLAT);
    glTranslatef(0,0,-5);glRotatef(28,1,0,0);
    glRotatef((GLfloat)(ns%6000000000ull)*0.00000006f,0,1,0);
    glBegin(GL_QUADS);
    for (unsigned f=0;f<6;f++) for (unsigned j=0;j<4;j++) {
        const GLfloat *p=points[faces[f][j]];
        if (gouraud) glNormal3f(p[0]*0.577350269f,p[1]*0.577350269f,p[2]*0.577350269f);
        else glNormal3f(normals[f][0],normals[f][1],normals[f][2]);
        /* RGB upload is top-first; t=0 is the portrait's top edge. */
        glTexCoord2f(j==1 || j==2 ? 1 : 0,j>=2 ? 0 : 1);
        glVertex3f(p[0],p[1],p[2]);
    }
    glEnd();finish(s);
}
static unsigned bounce(uint64_t ns,unsigned speed,unsigned offset,unsigned span)
{
    uint64_t p=((ns/1000000ull)*speed/1000+offset)%(span*2);
    return (unsigned)(p>span ? span*2-p : p);
}
void gfx_sprites(struct gfx_scene *s,uint64_t ns,const uint32_t *portrait)
{
    if (!begin(s,portrait)) return;
    unsigned w=s->width ? s->width : GFX_W,h=s->height ? s->height : GFX_H;
    glDisable(GL_DEPTH_TEST);glShadeModel(GL_FLAT);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();
    glScalef(2.0f/w,-2.0f/h,1);glTranslatef(-(GLfloat)w/2,-(GLfloat)h/2,0);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    glBegin(GL_QUADS);
    for (unsigned n=0;n<16;n++) {
        unsigned size=(20+n%4*4)*w/GFX_W;if (!size) size=1;
        if (size>=w || size>=h) continue;
        unsigned x=bounce(ns,35+n*7,n*37,w-size),y=bounce(ns,27+n*5,n*23,h-size);
        glTexCoord2f(0,0);glVertex3f((GLfloat)x,(GLfloat)y,0);
        glTexCoord2f(1,0);glVertex3f((GLfloat)(x+size),(GLfloat)y,0);
        glTexCoord2f(1,1);glVertex3f((GLfloat)(x+size),(GLfloat)(y+size),0);
        glTexCoord2f(0,1);glVertex3f((GLfloat)x,(GLfloat)(y+size),0);
    }
    glEnd();finish(s);
}
