/* CiukiOS backend for pinned Doomgeneric. GPL-2.0-or-later with Doomgeneric.
   Initial graphics/input port is silent; it never installs a sound driver. */
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "i_video.h"
#include "graphics_bridge.h"
#include <stdio.h>
#include <string.h>

extern boolean palette_changed;
extern struct color colors[256];
static unsigned char down_keys[512];
static unsigned last_focus=1;
static unsigned release_scan;

static unsigned char keycode(unsigned scan) {
    static const unsigned char ascii[58] = {
      0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9,
      'q','w','e','r','t','y','u','i','o','p','[',']',13,0,
      'a','s','d','f','g','h','j','k','l',';',39,'`',0,92,
      'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    unsigned s=scan&0x7f;
    switch(s) {
      case 0x48:return KEY_UPARROW;case 0x50:return KEY_DOWNARROW;
      case 0x4b:return KEY_LEFTARROW;case 0x4d:return KEY_RIGHTARROW;
      /* Doomgeneric binds actions to its own key IDs, as its upstream
         i_input.c scan table does. ASCII Space / KEY_RCTRL never reach
         key_use / key_fire, even though the raw PS/2 events arrive. */
      case 0x1d:return KEY_FIRE;case 0x39:return KEY_USE;
      case 0x38:return KEY_RALT;
      case 0x2a:case 0x36:return KEY_RSHIFT;
      case 0x3b:return KEY_F1;case 0x3c:return KEY_F2;
      case 0x3d:return KEY_F3;case 0x3e:return KEY_F4;
      case 0x3f:return KEY_F5;case 0x40:return KEY_F6;
      case 0x41:return KEY_F7;case 0x42:return KEY_F8;
      case 0x43:return KEY_F9;case 0x44:return KEY_F10;
      case 0x57:return KEY_F11;case 0x58:return KEY_F12;
      default:return s<sizeof(ascii)?ascii[s]:0;
    }
}
void DG_Init(void) {
    if(!cg_open()) I_Error("Ciuki graphics bridge not available (ABI1)");
    printf("[CGFX] Doom source port 320x200, silent backend\n");
}
void DG_DrawFrame(void) {
    unsigned i;
    if(cg_close_requested()) I_Quit();
    /* A repeated engine frame needs input/clock service, not another full
       desktop paint. The shared buffer is the last submitted image. */
    if(!palette_changed && !memcmp(cg_pixels(),DG_ScreenBuffer,64000)) {
        (void)cg_ticks();
        if(cg_failed()) I_Error("Ciuki graphics polling failed");
        return;
    }
    memcpy(cg_pixels(),DG_ScreenBuffer,64000);
    if(palette_changed) {
        unsigned char *p=cg_palette();
        for(i=0;i<256;i++) {
            p[i*3]=colors[i].r;p[i*3+1]=colors[i].g;p[i*3+2]=colors[i].b;
        }
    }
    if(!cg_present(palette_changed)) I_Error("Ciuki graphics presentation failed");
    palette_changed=false;
}
uint32_t DG_GetTicksMs(void) {
    uint32_t ticks=cg_ticks();
    if(cg_failed()) I_Error("Ciuki graphics clock poll failed");
    return ticks;
}
void DG_SleepMs(uint32_t milliseconds) {
    uint32_t start=DG_GetTicksMs();
    /* Clock polls service input without submitting duplicate game frames. */
    do {
        if(cg_close_requested()) I_Quit();
    } while((uint32_t)(DG_GetTicksMs()-start)<milliseconds);
}
int DG_GetKey(int *pressed,unsigned char *key) {
    unsigned scan,focus=(cg_info()->flags&CG_FLAG_FOCUSED)!=0;
    unsigned limit;
    int result;
    if(last_focus && !focus) release_scan=1;
    last_focus=focus;
    while(release_scan && release_scan<512) {
        scan=release_scan++;
        if(down_keys[scan]) {
            down_keys[scan]=0;*pressed=0;*key=keycode(scan);
            if(*key) return 1;
        }
    }
    release_scan=0;
    for(limit=0;limit<128;limit++) {
        result=cg_key(&scan,pressed);
        if(result<0) I_Error("Ciuki input bridge failed");
        if(!result) return 0;
        if(!focus) continue;
        *key=keycode(scan);
        if(!*key) continue;
        down_keys[scan]=*pressed!=0;
        return 1;
    }
    return 0;
}
void DG_SetWindowTitle(const char *title) { (void)title; }
int main(int argc,char **argv) {
    doomgeneric_Create(argc,argv);
    for(;;) doomgeneric_Tick();
}
