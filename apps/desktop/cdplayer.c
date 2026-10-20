/* Small Win32 CD player offered by AutoPlay, using the public MCI interface.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/user32.h>
#include "../../sdk/win32/audio/mci.h"
extern char *W32API GetCommandLineA(void);
extern HANDLE W32API GetModuleHandleA(const char *);
extern void W32API ExitProcess(uint32_t);
extern uint32_t W32API mciSendStringA(const char *, char *, uint32_t, uintptr_t);
extern BOOL W32API mciGetErrorStringA(uint32_t, char *, uint32_t);
extern int W32API MessageBoxA(HWND, const char *, const char *, UINT);
static HWND display;
static uint32_t track=1, tracks=1;
static void command(HWND h,const char *s) {
  uint32_t e=mciSendStringA(s,0,0,(uintptr_t)h);
  if (e) { char text[160]; if (!mciGetErrorStringA(e,text,sizeof(text))) text[0]=0; MessageBoxA(h,text,"CD Player",0x10); }
}
static void play(HWND h) {
  char s[]="play cd from 01 notify";
  s[13]=(char)('0'+track/10); s[14]=(char)('0'+track%10);
  /* Restrict a play to its audio track; next track may contain data. */
  char q[]="status cd length track 01", length[64];
  q[23]=(char)('0'+track/10); q[24]=(char)('0'+track%10);
  if (mciSendStringA(q,length,sizeof(length),0)) { command(h,s); return; }
  char p[128]="play cd from "; unsigned n=13;
  p[n++]=s[13]; p[n++]=s[14]; p[n++]=' '; p[n++]='t'; p[n++]='o'; p[n++]=' ';
  p[n++]=s[13]; p[n++]=s[14]; p[n++]=':';
  for (const char *t=length;*t && n<110;t++) p[n++]=*t;
  const char *tail=" notify"; while (*tail) p[n++]=*tail++; p[n]=0; command(h,p);
}
static LRESULT W32API window_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
  if (msg==0x111) {
    switch (wp&0xffff) {
    case 1: play(h); break;
    case 2: command(h,"pause cd"); break;
    case 3: command(h,"resume cd"); break;
    case 4: command(h,"stop cd"); break;
    case 5: if (track>1) track--; break;
    case 6: if (track<tracks) track++; break;
    case 7: command(h,"set cd door open"); break;
    }
    return 0;
  }
  if (msg==0x113 || msg==MM_MCINOTIFY) {
    char s[64]; if (!mciSendStringA("status cd position",s,sizeof(s),0)) SetWindowTextA(display,s);
    return 0;
  }
  if (msg==WM_CLOSE) { command(h,"close cd"); DestroyWindow(h); return 0; }
  if (msg==WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h,msg,wp,lp);
}
void W32API entry(void) {
  HANDLE instance=GetModuleHandleA(0);
  WNDCLASSA wc={0}; wc.lpfnWndProc=window_proc; wc.hInstance=instance;
  wc.hbrBackground=(HBRUSH)(uintptr_t)16; wc.lpszClassName="CiukiCDPlayer";
  if (!RegisterClassA(&wc)) ExitProcess(1);
  HWND h=CreateWindowExA(0,wc.lpszClassName,"CD Player",0x00ca0000u,100,100,440,150,0,0,instance,0);
  if (!h) ExitProcess(1);
  char open[]="open cdaudio!@0 alias cd";
  const char *s=GetCommandLineA();
  for (;*s;s++) if (s[0]=='-' && s[1]=='-' && s[2]=='s' && s[3]=='l' && s[4]=='o' && s[5]=='t' && s[6]=='=' && s[7]>='0' && s[7]<='3') { open[14]=s[7]; break; }
  uint32_t e=mciSendStringA(open,0,0,0);
  if (e) { command(h,open); ExitProcess(e); }
  command(h,"set cd time format tmsf");
  char count[32];
  if (!mciSendStringA("status cd number of tracks",count,sizeof(count),0)) {
    tracks=0; for (const char *p=count;*p>='0' && *p<='9';p++) tracks=tracks*10+(uint32_t)(*p-'0');
  }
  for (track=1;track<=tracks;track++) {
    char q[]="status cd type track 01", type[32]; q[21]=(char)('0'+track/10); q[22]=(char)('0'+track%10);
    if (!mciSendStringA(q,type,sizeof(type),0) && type[0]=='a') break;
  }
  display=CreateWindowExA(0,"STATIC","Audio CD",0x50000000u,12,12,400,24,h,0,instance,0);
  const char *labels[]={"Play","Pause","Resume","Stop","Previous","Next","Eject"};
  for (unsigned i=0;i<7;i++) CreateWindowExA(0,"BUTTON",labels[i],0x50010000u,8+(int)i*60,48,58,26,h,(HANDLE)(uintptr_t)(i+1),instance,0);
  ShowWindow(h,1); SetTimer(h,1,250,0); play(h);
  MSG m; while (GetMessageA(&m,0,0,0)>0) { TranslateMessage(&m); DispatchMessageA(&m); }
  ExitProcess(0);
}
