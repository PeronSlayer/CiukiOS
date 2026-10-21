/* Authored window and real DirectDraw COM calls. LGPL-2.1-or-later */
#ifndef CIUKI_F521B_SCENE_H
#define CIUKI_F521B_SCENE_H
#include <w32/ddraw.h>
#include "../win32_core/fixture.h"
typedef intptr_t(W32API *scene_proc)(uintptr_t,DWORD,uintptr_t,intptr_t);
struct scene_class {DWORD style;scene_proc procedure;int class_bytes,window_bytes;
    void *instance,*icon,*cursor,*brush;const char *menu,*name;};
extern uint16_t W32API RegisterClassA(const struct scene_class *);
extern uintptr_t W32API CreateWindowExA(DWORD,const char *,const char *,DWORD,int,int,int,int,uintptr_t,void *,void *,void *);
extern intptr_t W32API DefWindowProcA(uintptr_t,DWORD,uintptr_t,intptr_t);
extern BOOL W32API DestroyWindow(uintptr_t);
extern BOOL W32API UnregisterClassA(const char *,void *);
#define METHOD(o,n,type) ((type)((o)->vt[n]))
static uintptr_t scene_window;
static DD *scene_device;
static intptr_t W32API scene_procedure(uintptr_t h,DWORD m,uintptr_t w,intptr_t l)
{return DefWindowProcA(h,m,w,l);}
static int scene_begin(unsigned id)
{
    fixture_begin(id);
    struct scene_class c={.procedure=scene_procedure,.name="F521BScene"};
    CHECK(0,RegisterClassA(&c));
    scene_window=CreateWindowExA(0,c.name,"Ciuki DirectDraw fixture",0x10cf0000,32,32,640,480,0,0,0,0);
    CHECK(0,scene_window);
    CHECK(0,!DirectDrawCreate(0,&scene_device,0) && scene_device);
    if(!scene_device)return 0;
    CHECK(0,!METHOD(scene_device,20,DDRESULT(W32API *)(DD *,uintptr_t,DWORD))(
        scene_device,scene_window,DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN));
    return 1;
}
static int scene_mode(unsigned row,unsigned w,unsigned h,unsigned bpp)
{
    DDRESULT error=METHOD(scene_device,21,DDRESULT(W32API *)(DD *,DWORD,DWORD,DWORD))(scene_device,w,h,bpp);
    CHECK(row,!error);return !error;
}
static DD *scene_surface(unsigned row,unsigned caps,unsigned depth)
{
    DDDESC desc={.size=sizeof(desc),.flags=DDSD_CAPS,.caps=caps};
    if(!(caps&DDSCAPS_PRIMARYSURFACE)) {
        desc.flags|=DDSD_WIDTH|DDSD_HEIGHT|DDSD_PIXELFORMAT;
        desc.width=16;desc.height=16;
        desc.pixel=(DDPF){.size=32,.flags=DDPF_RGB|(depth==8 ? DDPF_PALETTEINDEXED8 : 0),.bits=depth,
            .red=depth==16 ? 0xf800 : 0xff0000,.green=depth==16 ? 0x7e0 : 0xff00,.blue=depth==16 ? 31 : 255};
    }
    DD *surface=0;
    CHECK(row,!METHOD(scene_device,6,DDRESULT(W32API *)(DD *,const DDDESC *,DD **,void *))(
        scene_device,&desc,&surface,0) && surface);
    return surface;
}
static void scene_release(DD *object)
{if(object)METHOD(object,2,DWORD(W32API *)(DD *))(object);}
static void scene_end(void)
{
    if(scene_device) {
        CHECK(0,!METHOD(scene_device,19,DDRESULT(W32API *)(DD *))(scene_device));
        CHECK(0,!METHOD(scene_device,20,DDRESULT(W32API *)(DD *,uintptr_t,DWORD))(scene_device,scene_window,DDSCL_NORMAL));
        scene_release(scene_device);
    }
    if(scene_window)CHECK(0,DestroyWindow(scene_window));
    CHECK(0,UnregisterClassA("F521BScene",0));
}
#endif
