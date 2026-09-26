/* CiukiOS indexed surface/input adapter for Wolf4SDL. GPL-2.0-or-later.
   This is a source compatibility layer, not an SDL binary implementation.
   Audio, joystick, mouse capture, fullscreen changes and BMP export are absent. */
#include "SDL.h"
#include "graphics_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern void Quit(const char *,...);
static SDL_Window native_window={320,200};
static SDL_Renderer renderer;
static SDL_Texture texture={320,200};
static SDL_Surface *front;
static SDL_Color palette[256];
static int palette_dirty=1,frame_dirty=1,initialized,queued;
static SDL_Event queued_event;
static unsigned char held[512];
static int mods,release_scan;

int SDL_Init(Uint32) {
    if(initialized) return 0;
    if(!cg_open()) return -1;
    initialized=1;
    puts("[CGFX] Wolf source port 320x200, silent backend");
    return 0;
}
void SDL_Quit(void) { initialized=0; }
const char *SDL_GetError(void) { return "Ciuki indexed adapter: operation unavailable"; }
Uint32 SDL_GetTicks(void) {
    Uint32 now=cg_ticks();
    if(cg_failed()) Quit("Ciuki graphics clock failed");
    if(cg_close_requested()) Quit(NULL);
    return now;
}
void SDL_Delay(Uint32 ms) { Uint32 start=SDL_GetTicks();while(SDL_GetTicks()-start<ms) {} }
SDL_Window *SDL_CreateWindow(const char *,int,int,int w,int h,Uint32) {
    if(w!=320 || h!=200) return NULL;
    return &native_window;
}
SDL_Renderer *SDL_CreateRenderer(SDL_Window *,int,Uint32) { return &renderer; }
SDL_Texture *SDL_CreateTexture(SDL_Renderer *,Uint32,int,int w,int h) { return w==320&&h==200?&texture:NULL; }
void SDL_SetWindowMinimumSize(SDL_Window *,int,int) {}
void SDL_SetWindowSize(SDL_Window *,int,int) {}
int SDL_SetWindowFullscreen(SDL_Window *,Uint32 flags) { return flags?-1:0; }
void SDL_GetWindowSize(SDL_Window *,int *w,int *h) { *w=320;*h=200; }
Uint32 SDL_GetWindowID(SDL_Window *) { return 1; }
Uint32 SDL_GetWindowPixelFormat(SDL_Window *) { return 8; }
int SDL_RenderSetLogicalSize(SDL_Renderer *,int w,int h) { return w==320&&h==200?0:-1; }
int SDL_SetHint(const char *,const char *) { return 1; }
int SDL_PixelFormatEnumToMasks(Uint32,int *b,Uint32 *r,Uint32 *g,Uint32 *bl,Uint32 *a) {
    *b=8;*r=*g=*bl=*a=0;return 1;
}
SDL_Palette *SDL_AllocPalette(int n) {
    SDL_Palette *p=(SDL_Palette *)calloc(1,sizeof(*p));if(!p)return NULL;
    p->colors=(SDL_Color *)calloc(n,sizeof(SDL_Color));p->ncolors=n;
    if(!p->colors){free(p);return NULL;}return p;
}
void SDL_FreePalette(SDL_Palette *p) { if(p){free(p->colors);free(p);} }
int SDL_SetPaletteColors(SDL_Palette *p,const SDL_Color *c,int start,int count) {
    if(!p || start<0 || count<0 || start+count>p->ncolors)return -1;
    memcpy(p->colors+start,c,count*sizeof(*c));return 0;
}
SDL_Surface *SDL_CreateRGBSurface(Uint32,int w,int h,int b,Uint32,Uint32,Uint32,Uint32) {
    if(w<=0||h<=0||b!=8)return NULL;
    SDL_Surface *s=(SDL_Surface *)calloc(1,sizeof(*s));if(!s)return NULL;
    s->w=w;s->h=h;s->pitch=w;s->pixels=calloc(w,h);
    s->format=(SDL_PixelFormat *)calloc(1,sizeof(*s->format));
    if(!s->pixels||!s->format){SDL_FreeSurface(s);return NULL;}
    s->format->BitsPerPixel=8;s->format->BytesPerPixel=1;
    s->format->palette=SDL_AllocPalette(256);
    if(!s->format->palette){SDL_FreeSurface(s);return NULL;}
    return s;
}
void SDL_FreeSurface(SDL_Surface *s) {
    if(!s)return;free(s->pixels);
    if(s->format){SDL_FreePalette(s->format->palette);free(s->format);}free(s);
}
int SDL_LockSurface(SDL_Surface *) { return 0; }
void SDL_UnlockSurface(SDL_Surface *) {}
int SDL_SetSurfacePalette(SDL_Surface *s,SDL_Palette *p) {
    if(!s||!p||p->ncolors!=256)return -1;
    return SDL_SetPaletteColors(s->format->palette,p->colors,0,256);
}
int SDL_BlitSurface(SDL_Surface *s,const SDL_Rect *sr,SDL_Surface *d,SDL_Rect *dr) {
    if(!s||!d)return -1;
    int sx=sr?sr->x:0,sy=sr?sr->y:0,w=sr?sr->w:s->w,h=sr?sr->h:s->h;
    int dx=dr?dr->x:0,dy=dr?dr->y:0;
    if(sx<0){dx-=sx;w+=sx;sx=0;}if(sy<0){dy-=sy;h+=sy;sy=0;}
    if(dx<0){sx-=dx;w+=dx;dx=0;}if(dy<0){sy-=dy;h+=dy;dy=0;}
    if(w>s->w-sx)w=s->w-sx;if(w>d->w-dx)w=d->w-dx;
    if(h>s->h-sy)h=s->h-sy;if(h>d->h-dy)h=d->h-dy;
    if(w<=0||h<=0)return 0;
    for(int y=0;y<h;y++)memmove((char*)d->pixels+(dy+y)*d->pitch+dx,(char*)s->pixels+(sy+y)*s->pitch+sx,w);
    SDL_SetSurfacePalette(d,s->format->palette);front=d;return 0;
}
int SDL_FillRect(SDL_Surface *s,const SDL_Rect *r,Uint32 color) {
    if(!s)return -1;
    int x=r?r->x:0,y=r?r->y:0,w=r?r->w:s->w,h=r?r->h:s->h;
    if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
    if(w>s->w-x)w=s->w-x;if(h>s->h-y)h=s->h-y;
    if(w>0&&h>0)for(int row=0;row<h;row++)memset((char*)s->pixels+(row+y)*s->pitch+x,color,w);
    return 0;
}
Uint32 SDL_MapRGB(SDL_PixelFormat *p,Uint8 r,Uint8 g,Uint8 b) {
    unsigned best=0;long distance=0x7fffffff;
    for(int i=0;i<p->palette->ncolors;i++){
        SDL_Color c=p->palette->colors[i];long dr=(int)c.r-r,dg=(int)c.g-g,db=(int)c.b-b;
        long d=dr*dr+dg*dg+db*db;if(d<distance){distance=d;best=i;}
    }return best;
}
int SDL_UpdateTexture(SDL_Texture *,const SDL_Rect *,const void *p,int pitch) {
    if(!p||pitch<320)return -1;
    frame_dirty=0;
    for(int y=0;y<200;y++){
        const char *row=(const char*)p+y*pitch;
        if(memcmp(cg_pixels()+y*320,row,320))frame_dirty=1;
        memcpy(cg_pixels()+y*320,row,320);
    }
    if(front && memcmp(palette,front->format->palette->colors,sizeof(palette))){
        memcpy(palette,front->format->palette->colors,sizeof(palette));palette_dirty=1;
    }return 0;
}
int SDL_RenderClear(SDL_Renderer *) { return 0; }
int SDL_RenderCopy(SDL_Renderer *,SDL_Texture *,const SDL_Rect *,const SDL_Rect *) { return 0; }
void SDL_RenderPresent(SDL_Renderer *) {
    if(cg_close_requested())Quit(NULL);
    if(!frame_dirty&&!palette_dirty){SDL_GetTicks();return;}
    if(palette_dirty)for(int i=0;i<256;i++){
        cg_palette()[i*3]=palette[i].r;cg_palette()[i*3+1]=palette[i].g;cg_palette()[i*3+2]=palette[i].b;
    }
    if(!cg_present(palette_dirty))Quit("Ciuki graphics presentation failed");
    palette_dirty=0;
}
int SDL_SaveBMP(SDL_Surface *,const char *) { return -1; }
static int keycode(unsigned scan) {
    static const unsigned char ascii[58]={0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9,
      'q','w','e','r','t','y','u','i','o','p','[',']',13,0,'a','s','d','f','g','h','j','k','l',';',39,'`',0,92,
      'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
    static const int fn[12]={SDLK_F1,SDLK_F2,SDLK_F3,SDLK_F4,SDLK_F5,SDLK_F6,SDLK_F7,SDLK_F8,SDLK_F9,SDLK_F10,SDLK_F11,SDLK_F12};
    unsigned s=scan&127;
    if(s>=0x3b&&s<=0x44)return fn[s-0x3b];if(s==0x57||s==0x58)return fn[s-0x57+10];
    switch(s){case 0x48:return SDLK_UP;case 0x50:return SDLK_DOWN;case 0x4b:return SDLK_LEFT;case 0x4d:return SDLK_RIGHT;
      case 0x1d:return SDLK_LCTRL;case 0x38:return SDLK_LALT;case 0x2a:case 0x36:return SDLK_LSHIFT;case 0x3a:return SDLK_CAPSLOCK;}
    return s<58?ascii[s]:0;
}
int SDL_PollEvent(SDL_Event *e) {
    unsigned scan;int down,result;
    SDL_GetTicks();
    if(queued){*e=queued_event;queued=0;return 1;}
    if(!(cg_info()->flags&CG_FLAG_FOCUSED)&&!release_scan)release_scan=1;
    while(release_scan&&release_scan<512){scan=release_scan++;if(held[scan]){held[scan]=0;down=0;goto event;}}
    release_scan=0;
    for(int i=0;i<128;i++){
        result=cg_key(&scan,&down);if(result<0)Quit("Ciuki input failed");if(!result)return 0;
        if(!(cg_info()->flags&CG_FLAG_FOCUSED))continue;
        if(!keycode(scan))continue;held[scan]=(unsigned char)down;goto event;
    }return 0;
event:
    memset(e,0,sizeof(*e));e->type=down?SDL_KEYDOWN:SDL_KEYUP;e->key.keysym.scancode=scan;e->key.keysym.sym=keycode(scan);
    if(e->key.keysym.sym==SDLK_LSHIFT){if(down)mods|=KMOD_SHIFT;else mods&=~KMOD_SHIFT;}
    if(e->key.keysym.sym==SDLK_LCTRL){if(down)mods|=KMOD_LCTRL;else mods&=~KMOD_LCTRL;}
    if(e->key.keysym.sym==SDLK_LALT){if(down)mods|=KMOD_LALT;else mods&=~KMOD_LALT;}
    e->key.keysym.mod=mods;return 1;
}
int SDL_WaitEvent(SDL_Event *e) { while(!SDL_PollEvent(e))SDL_Delay(1);return 1; }
int SDL_PushEvent(SDL_Event *e) { if(queued)return -1;queued_event=*e;queued=1;return 1; }
SDL_Keymod SDL_GetModState(void) { return mods; }
int SDL_SetRelativeMouseMode(int enabled) { return enabled?-1:0; }
Uint32 SDL_GetRelativeMouseState(int *x,int *y) { if(x)*x=0;if(y)*y=0;return 0; }
void SDL_WarpMouseInWindow(SDL_Window *,int,int) {}
int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int) { return NULL; }
void SDL_JoystickClose(SDL_Joystick *) {}
int SDL_JoystickEventState(int) { return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *) { return 0; }
int SDL_JoystickNumHats(SDL_Joystick *) { return 0; }
int SDL_JoystickGetAxis(SDL_Joystick *,int) { return 0; }
int SDL_JoystickGetButton(SDL_Joystick *,int) { return 0; }
int SDL_JoystickGetHat(SDL_Joystick *,int) { return 0; }
void SDL_JoystickUpdate(void) {}
