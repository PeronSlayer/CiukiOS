/* Offscreen production scenes and independent GL pixel assertions.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../apps/gfxdemo/gfxdemo.h"
#include "../../apps/gfxdemo/fps.h"
#include <GL/gl.h>
#include <ciuki/gl.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now(void)
{
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static void triangle(float z,float r,float g,float b)
{
    glColor3f(r,g,b);glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f,-0.8f,z);glVertex3f(0.8f,-0.8f,z);glVertex3f(0,0.8f,z);glEnd();
}
static void semantics(void)
{
    const uint32_t guard=0xdeadbeef;
    uint32_t *p=malloc((67*49+2)*4);assert(p);
    for (unsigned i=0;i<67*49+2;i++) p[i]=guard;
    assert(ciuki_gl_bind(NULL,64,48,64)==-EINVAL);
    assert(ciuki_gl_bind(p,0,48,64)==-EINVAL);
    assert(ciuki_gl_bind(p,1601,48,1601)==-EINVAL);
    assert(ciuki_gl_bind(p,64,1201,64)==-EINVAL);
    assert(ciuki_gl_bind(p,64,48,63)==-EINVAL);
    assert(!ciuki_gl_bind(p+1,65,49,67));
    glClearColor(1,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);ciuki_gl_finish();
    assert(p[0]==guard && p[67*49+1]==guard);
    for (unsigned y=0;y<49;y++) {
        for (unsigned x=0;x<65;x++) assert(p[1+y*67+x]==0xff0000);
        assert(p[1+y*67+65]==guard && p[1+y*67+66]==guard);
    }
    glEnable(GL_DEPTH_TEST);glDepthMask(1);
    /* Rebinding packed and padded storage retains depth state. */
    assert(!ciuki_gl_bind(p,64,48,64));
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    triangle(-0.5f,0,1,0);triangle(0.5f,1,0,0);ciuki_gl_finish();
    assert((p[24*64+32]&0xff00ffu)==0 && (p[24*64+32]&0xff00u)>=0xf000u);
    glDisable(GL_DEPTH_TEST);triangle(0.8f,0,0,1);ciuki_gl_finish();assert((p[24*64+32]&0xffff00u)==0 && (p[24*64+32]&0xffu)>=0xf0u);
    glBindTexture(-1,0);glDrawBuffer(-1);
    assert(glGetError()==GL_INVALID_ENUM);assert(glGetError()==GL_NO_ERROR);
    /* f7-14: the shared engine's texture objects and status report. */
    GLuint names[8];glGenTextures(8,names);
    for (unsigned i=0;i<8;i++) { assert(names[i]); for (unsigned j=0;j<i;j++) assert(names[i]!=names[j]); }
    glDeleteTextures(8,names);assert(!glIsTexture(names[0]));
    struct ciuki_gl_status status={.size=sizeof(status)};
    assert(!ciuki_gl_status(&status) && status.backend==CIUKI_GL_BACKEND_SOFTWARE && status.frames>=3 &&
        status.software_frames==status.frames && !status.hardware_draws && status.software_draws);
    assert(ciuki_gl_set_backend(CIUKI_GL_BACKEND_HARDWARE)==-ENODEV && !ciuki_gl_set_backend(CIUKI_GL_BACKEND_AUTO));
    assert(ciuki_gl_get_proc_address("glActiveTextureARB") && !ciuki_gl_get_proc_address("glNothing"));
    unsigned char *rgb=malloc(256*256*3);assert(rgb);
    for (unsigned i=0;i<256*256;i++) { rgb[i*3]=255;rgb[i*3+1]=0;rgb[i*3+2]=255; }
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,(int)texture);
    glTexImage2D(GL_TEXTURE_2D,0,3,256,256,0,GL_RGB,GL_UNSIGNED_BYTE,rgb);free(rgb);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glEnable(GL_TEXTURE_2D);glColor3f(1,1,1);glBegin(GL_QUADS);
    glTexCoord2f(0,0);glVertex3f(-1,-1,0);glTexCoord2f(1,0);glVertex3f(1,-1,0);
    glTexCoord2f(1,1);glVertex3f(1,1,0);glTexCoord2f(0,1);glVertex3f(-1,1,0);glEnd();
    ciuki_gl_finish();assert((p[24*64+32]&0xff00u)==0 && (p[24*64+32]&0xff0000u)>=0xf00000u &&
        (p[24*64+32]&0xffu)>=0xf0u);assert(glGetError()==GL_NO_ERROR);
    unsigned gen=ciuki_gl_generation();ciuki_gl_close();ciuki_gl_close();
    assert(!ciuki_gl_bind(p,1,1,1) && ciuki_gl_generation()==gen+1);
    glClearColor(0,1,0,0);glClear(GL_COLOR_BUFFER_BIT);ciuki_gl_finish();assert(p[0]==0xff00);
    ciuki_gl_close();free(p);
    struct gfx_fps *fps=calloc(1,sizeof(*fps));assert(fps);gfx_fps_start(fps,100,10);
    for (unsigned i=1;i<=100;i++) gfx_fps_frame(fps,100+i*10000000ull);
    gfx_fps_frame(fps,10);gfx_fps_frame(fps,fps->end+1);
    assert(fps->frames==100 && gfx_fps_p95(fps)==10000000);
    gfx_fps_start(fps,100,10);assert(!gfx_fps_p95(fps));free(fps);
    puts("PASS GL clear/RGB, depth occlusion, depth disable, errors, odd width/stride canaries, resize state, close/reopen, FPS percentile");
}
static void render(struct gfx_scene *s,unsigned scene,uint64_t phase,const uint32_t *portrait)
{
    if (scene<2) gfx_cube(s,phase,portrait,(int)scene);
    else gfx_sprites(s,phase,portrait);
    assert(!s->error);
}
int main(int argc,char **argv)
{
    assert(argc==3 || argc==5);semantics();
    uint32_t *portrait=malloc(256*256*4);struct gfx_scene *s=calloc(1,sizeof(*s));assert(portrait&&s);
    FILE *f=fopen(argv[1],"rb");assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    s->width=640;s->height=480;
    const char *names[]={"cube-flat","cube-smooth","tiles"};
    if (argc==3) {
        for (unsigned scene=0;scene<3;scene++) {
            render(s,scene,1234567890ull,portrait);
            char path[1024];assert(snprintf(path,sizeof(path),"%s/%s.xrgb",argv[2],names[scene])<(int)sizeof(path));
            f=fopen(path,"wb");assert(f && fwrite(s->pixels,4,640*480,f)==640*480 && !fclose(f));
            uint32_t *first=malloc(640*480*4);assert(first);memcpy(first,s->pixels,640*480*4);
            render(s,scene,1234567890ull,portrait);assert(!memcmp(first,s->pixels,640*480*4));
            render(s,scene,2234567890ull,portrait);assert(memcmp(first,s->pixels,640*480*4));free(first);
        }
        puts("PASS production OpenGL cube flat/smooth and tiles: stable pose and elapsed-time motion, 640x480");
    } else {
        assert(!strcmp(argv[3],"--benchmark"));unsigned seconds=(unsigned)strtoul(argv[4],NULL,10);assert(seconds && seconds<=60);
        size_t cap=1048576;uint64_t *samples=malloc(cap*sizeof(*samples));assert(samples);
        for (unsigned scene=0;scene<3;scene++) {
            render(s,scene,0,portrait); /* Exclude texture upload/context creation. */
            uint64_t begin=now(),end=begin,limit=begin+(uint64_t)seconds*1000000000ull;
            unsigned count=0;
            while (end<limit) {
                uint64_t start=now();render(s,scene,start-begin,portrait);end=now();
                assert(count<cap);samples[count++]=end-start;
            }
            qsort(samples,count,sizeof(*samples),gfx_fps_compare);
            uint64_t elapsed=end-begin,fps100=(uint64_t)count*100000000000ull/elapsed;
            printf("GL_HOST scene=%s source=640x480 seconds=%u frames=%u average_fps=%llu.%02llu frame_us=%llu p95_us=%llu\n",
                names[scene],seconds,count,(unsigned long long)(fps100/100),(unsigned long long)(fps100%100),
                (unsigned long long)(elapsed/count/1000),(unsigned long long)(samples[(count*95u+99u)/100u-1]/1000));
            /* A sanity bound only, allowing slow sanitizer/shared hosts. */
            assert(elapsed/count<250000000ull);
        }
        free(samples);
    }
    ciuki_gl_close();free(portrait);free(s);return 0;
}
