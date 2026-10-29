/* WinMM joystick queries/capture on the native USB/game-port controller set.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "audio.h"
#include <w32/kernel32.h>
extern HANDLE W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(HANDLE,const char *);
extern uint32_t W32API timeSetEvent(uint32_t,uint32_t,uintptr_t,uintptr_t,uint32_t);
extern uint32_t W32API timeKillEvent(uint32_t);
static uint32_t snapshot(uint32_t id,struct ciuki_joystick_control *q) {
  if(id>=16) return 165;
  *q=(struct ciuki_joystick_control){.version=1,.size=sizeof(*q),.operation=CIUKI_JOY_NEXT};
  for(unsigned i=0;i<=id;i++) {
    struct ciuki_w32_system_args a={.header=K32_HEADER(W32_SYSTEM,a),.action=W32_S_JOYSTICK,
        .low=K32_PTR(q),.extra=sizeof(*q)};
    if(w32_ciuki_call(&a,sizeof(a)) || a.error || !a.result) return 167;
  }
  return q->attached ? 0 : 167;
}
static uint32_t hid(const struct ciuki_joystick_control *q,unsigned operation,void *out,unsigned size) {
  w32_fill(out,0,size);uint32_t *p=out;
  p[0]=CIUKI_HID_VERSION;p[1]=size;p[2]=q->device;p[3]=q->generation;
  struct ciuki_w32_system_args a={.header=K32_HEADER(W32_SYSTEM,a),.action=W32_S_JOYSTICK,
      .value=operation,.low=K32_PTR(out),.extra=size};
  return w32_ciuki_call(&a,sizeof(a)) || a.error || !a.result ? 167 : 0;
}
static unsigned population(uint32_t bits) {
  unsigned n=0;while(bits) {n+=bits&1;bits>>=1;}return n;
}
static uint32_t axis(int32_t n) {
  if(n< -32767) n=-32767;
  if(n>32767) n=32767;
  return (uint32_t)(((int64_t)n+32767)*65535/65534);
}
uint32_t W32API joyGetNumDevs(void) {return 16;}
uint32_t W32API joyGetPos(uint32_t id,void *out) {
  if(!out) return 165;
  struct ciuki_joystick_control q;uint32_t e=snapshot(id,&q);if(e) return e;
  struct ciuki_hid_state state;
  e=hid(&q,2,&state,sizeof(state));if(e || !state.attached) return 167;
  for(unsigned i=0;i<3;i++) w32_put32((uint8_t *)out+4*i,axis(state.axes[i]));
  w32_put32((uint8_t *)out+12,state.buttons);return 0;
}
uint32_t W32API joyGetPosEx(uint32_t id,void *out) {
  if(!out || w32_u32(out)!=52) return 165;
  uint8_t *p=out;uint32_t flags=w32_u32(p+4);
  if(flags&~0xfffu) return 165;
  struct ciuki_joystick_control q;uint32_t e=snapshot(id,&q);if(e) return e;
  if((flags&0x100) && q.device<CIUKI_GAMEPORT_FIRST) return 165;
  struct ciuki_hid_state state;
  e=hid(&q,2,&state,sizeof(state));if(e || !state.attached) return 167;
  for(unsigned i=0;i<6;i++) if(flags&(1u<<i)) {
    int32_t value=state.axes[i];
    w32_put32(p+8+4*i,(flags&0x100) && i<4 ? (uint32_t)q.raw[i] : axis(value));
  }
  if(flags&0x80) {
    w32_put32(p+32,state.buttons);
    unsigned first=0;
    while(first<32 && !(state.buttons&(1u<<first))) first++;
    w32_put32(p+36,first<32 ? first+1 : 0);
  }
  if(flags&(0x40|0x200)) w32_put32(p+40,state.hats[0]<0 ? 65535u : (uint32_t)state.hats[0]*4500);
  return 0;
}
static uint32_t caps(uintptr_t id,void *out,uint32_t n,int wide) {
  if(!out) return 165;
  uint8_t b[728]={0};
  unsigned name_bytes=wide ? 64 : 32, base=4+name_bytes;
  const char *name="CiukiOS joystick";
  if((uint32_t)id!=UINT32_MAX) {
    struct ciuki_joystick_control q;uint32_t e=snapshot((uint32_t)id,&q);if(e) return e;
    struct ciuki_hid_caps c;
    e=hid(&q,1,&c,sizeof(c));if(e) return e;
    unsigned axes=population(c.axes&63u),buttons=population(c.buttons);
    w32_put16(b,1);w32_put16(b+2,1);
    for(unsigned i=0;i<3;i++) {w32_put32(b+base+i*8,0);w32_put32(b+base+i*8+4,65535);}
    w32_put32(b+base+24,buttons);
    w32_put32(b+base+28,1);w32_put32(b+base+32,1000);
    for(unsigned i=0;i<3;i++) w32_put32(b+base+40+i*8,65535);
    w32_put32(b+base+60,((c.axes&4) ? 1u : 0)|((c.axes&8) ? 2u : 0)|((c.axes&16) ? 4u : 0)|((c.axes&32) ? 8u : 0)|(c.hats ? 0x50u : 0));
    w32_put32(b+base+64,6);w32_put32(b+base+68,axes);w32_put32(b+base+72,32);
  }
  for(unsigned i=0;name[i];i++) {
    if(wide) w32_put16(b+4+i*2,name[i]);else b[4+i]=(uint8_t)name[i];
  }
  const char *key="CiukiOS";
  for(unsigned i=0;key[i];i++) {
    if(wide) w32_put16(b+base+76+i*2,key[i]);else b[base+76+i]=(uint8_t)key[i];
  }
  uint32_t size=wide ? 728 : 404;
  w32_copy(out,b,n<size ? n : size);return 0;
}
uint32_t W32API joyGetDevCapsA(uintptr_t id,void *p,uint32_t n) {return caps(id,p,n,0);}
uint32_t W32API joyGetDevCapsW(uintptr_t id,void *p,uint32_t n) {return caps(id,p,n,1);}
struct capture {
  HANDLE window;uint32_t timer,changed,last[4],threshold;
};
static struct capture captures[16];
static int distance(uint32_t a,uint32_t b,uint32_t threshold) {return a>b ? a-b>threshold : b-a>threshold;}
static void W32API poll(uintptr_t timer,uint32_t message,uintptr_t user,uintptr_t p1,uintptr_t p2) {
  (void)message;(void)p1;(void)p2;
  typedef BOOL(W32API *post_fn)(HANDLE,DWORD,uintptr_t,intptr_t);
  typedef BOOL(W32API *valid_fn)(HANDLE);
  post_fn post=(post_fn)GetProcAddress(GetModuleHandleA("user32.dll"),"PostMessageA");
  valid_fn valid=(valid_fn)GetProcAddress(GetModuleHandleA("user32.dll"),"IsWindow");
  unsigned id=(unsigned)user;uint32_t pos[4];
  if(id>=16 || !post || !valid) return;
  CiukiAudioLock();struct capture c=captures[id];CiukiAudioUnlock();
  if(c.timer!=timer || !c.window) return;
  if(!valid(c.window)) {
    timeKillEvent((uint32_t)timer);
    CiukiAudioLock();if(captures[id].timer==timer) captures[id].window=0;CiukiAudioUnlock();return;
  }
  if(joyGetPos(id,pos)) return;
  uint32_t xy=(pos[0]&65535)|(pos[1]<<16),changed=(c.last[3]^pos[3])&15;
  uint32_t buttons=pos[3]&15;
  if(!c.changed || distance(pos[0],c.last[0],c.threshold) || distance(pos[1],c.last[1],c.threshold))
    post(c.window,0x3a0u+id,buttons,xy);
  if(!c.changed || distance(pos[2],c.last[2],c.threshold)) post(c.window,0x3a2u+id,buttons,pos[2]);
  if(changed&buttons) post(c.window,0x3b5u+id,buttons|((changed&buttons)<<8),xy);
  if(changed&~buttons) post(c.window,0x3b7u+id,buttons|((changed&~buttons)<<8),xy);
  CiukiAudioLock();
  if(captures[id].timer==timer) w32_copy(captures[id].last,pos,sizeof(pos));
  CiukiAudioUnlock();
}
uint32_t W32API joyReleaseCapture(uint32_t id) {
  if(id>=16) return 165;
  CiukiAudioLock();uint32_t timer=captures[id].timer;
  captures[id].timer=0;captures[id].window=0;CiukiAudioUnlock();
  if(timer) timeKillEvent(timer);return 0;
}
uint32_t W32API joySetCapture(HANDLE window,uint32_t id,uint32_t period,BOOL changed) {
  if(!window || id>=16 || !period) return 165;
  uint32_t pos[4],e=joyGetPos(id,pos);if(e) return e;
  joyReleaseCapture(id);
  CiukiAudioLock();captures[id].window=window;captures[id].changed=!!changed;
  w32_copy(captures[id].last,pos,sizeof(pos));
  captures[id].timer=timeSetEvent(period,0,(uintptr_t)poll,id,1);
  e=captures[id].timer ? 0 : 166;
  if(e) captures[id].window=0;
  CiukiAudioUnlock();return e;
}
uint32_t W32API joyGetThreshold(uint32_t id,uint32_t *p) {
  if(id>=16 || !p) return 165;
  CiukiAudioLock();*p=captures[id].threshold;CiukiAudioUnlock();return 0;
}
uint32_t W32API joySetThreshold(uint32_t id,uint32_t n) {
  if(id>=16) return 165;
  CiukiAudioLock();captures[id].threshold=n;CiukiAudioUnlock();return 0;
}
