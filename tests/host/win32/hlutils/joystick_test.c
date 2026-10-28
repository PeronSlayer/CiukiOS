/* Native query and WinMM timer/window boundary fixture. LGPL-2.1-or-later. */
#include <w32/kernel32.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const void *ptrs[128];static unsigned np;
static DWORD pointer(const void *p) {assert(np<128);ptrs[++np]=p;return np;}
#undef K32_PTR
#define K32_PTR(p) pointer(p)
#include "../../../../sdk/win32/audio/joystick.c"
static unsigned plugged=1,alive=1,posts,killed,button=3;static int32_t position=-32767;
static uintptr_t timer_callback,timer_user;
int32_t w32_ciuki_call(void *record,uint32_t bytes) {
  struct ciuki_w32_system_args *a=record;
  assert(bytes==sizeof(*a) && a->action==W32_S_JOYSTICK);
  if(a->value==1) {
    struct ciuki_hid_caps *c=(void *)ptrs[a->low];assert(a->extra==sizeof(*c));
    c->attached=1;c->classes=CIUKI_HID_JOYSTICK;c->axes=15;c->buttons=15;a->result=1;return 0;
  }
  if(a->value==2) {
    struct ciuki_hid_state *state=(void *)ptrs[a->low];assert(a->extra==sizeof(*state));
    state->attached=1;state->buttons=button;state->axes[0]=position;state->axes[1]=32767;
    for(unsigned i=0;i<CIUKI_HID_HATS;i++) state->hats[i]=-1;
    a->result=1;return 0;
  }
  struct ciuki_joystick_control *q=(void *)ptrs[a->low];
  assert(q->operation==CIUKI_JOY_NEXT);
  if(!plugged || q->device==257) {a->error=1167;return 0;}
  q->device=q->device ? 257 : 1;q->generation=5;q->attached=1;q->axes=15;q->buttons=button;
  q->position[0]=position;q->position[1]=32767;q->position[2]=0;q->position[3]=123;
  a->result=1;return 0;
}
void W32API CiukiAudioLock(void) {}
void W32API CiukiAudioUnlock(void) {}
HANDLE W32API GetModuleHandleA(const char *name) {assert(!strcmp(name,"user32.dll"));return (void *)1;}
static BOOL W32API valid(HANDLE window) {assert(window==(void *)7);return (BOOL)alive;}
static BOOL W32API post(HANDLE window,DWORD message,uintptr_t wp,intptr_t lp) {
  assert(window==(void *)7 && message>=0x3a0 && message<=0x3c6);(void)wp;(void)lp;posts++;return 1;
}
void *W32API GetProcAddress(HANDLE h,const char *name) {(void)h;return !strcmp(name,"IsWindow") ? (void *)valid : (void *)post;}
uint32_t W32API timeSetEvent(uint32_t period,uint32_t resolution,uintptr_t cb,uintptr_t user,uint32_t flags) {
  assert(period==20 && !resolution && flags==1);timer_callback=cb;timer_user=user;return 9;
}
uint32_t W32API timeKillEvent(uint32_t id) {assert(id==9);killed++;return 0;}
int main(void) {
  assert(joyGetNumDevs()==16);uint32_t p[13]={52,0xff};
  assert(!joyGetPosEx(0,p) && p[2]==0 && p[3]==65535 && p[8]==3 && p[9]==1 && p[10]==65535);
  p[1]=0x101;assert(joyGetPosEx(0,p)==165);
  p[1]=1;p[3]=0xabcdef;assert(!joyGetPosEx(0,p) && p[3]==0xabcdef);
  p[0]=12;assert(joyGetPosEx(0,p)==165);assert(joyGetPos(16,p)==165);
  uint8_t caps[404];assert(!joyGetDevCapsA(1,caps,sizeof(caps)));
  assert(w32_u32(caps+36+68)==4 && w32_u32(caps+36+24)==4);
  assert(joyGetDevCapsA(2,caps,sizeof(caps))==167);
  assert(!joySetCapture((void *)7,0,20,1));assert(timer_callback);
  void(W32API *tick)(uintptr_t,uint32_t,uintptr_t,uintptr_t,uintptr_t)=(void *)timer_callback;
  tick(9,0,timer_user,0,0);assert(!posts);
  position=100;button=1;tick(9,0,timer_user,0,0);assert(posts==2);
  assert(!joyReleaseCapture(0) && killed==1);
  assert(!joySetCapture((void *)7,0,20,0));alive=0;tick(9,0,timer_user,0,0);assert(killed==2);
  plugged=0;assert(joyGetPos(0,p)==167 && joySetCapture((void *)7,0,20,0)==167);
  puts("PASS USB/analog enumeration, normalized axes, flags, caps, capture, release and detach");
}
