/* Progress and Cancel use the same USER controls as installer dialogs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
#include <w32/user32.h>
struct shell_progress {HWND window,label;BOOL cancelled,simple;};
static LRESULT W32API procedure(HWND h,UINT message,WPARAM wp,LPARAM lp){
  struct shell_progress *p=GetPropA(h,"CiukiFileProgress");
  if(message==0x81){
    CREATESTRUCTA *c=(void *)lp;p=c->lpCreateParams;
    if(!SetPropA(h,"CiukiFileProgress",p))return 0;return 1;
  }
  if(p && (message==0x10 || (message==0x111 && (wp&65535)==2))){p->cancelled=1;return 0;}
  if(message==2)RemovePropA(h,"CiukiFileProgress");
  return DefWindowProcA(h,message,wp,lp);
}
struct shell_progress *shell_progress_begin(const SHELL_FILEOP *a){
  if(a->flags&4)return 0;
  struct shell_progress *p=shell_alloc(sizeof(*p));if(!p)return 0;
  WNDCLASSA cls={.lpfnWndProc=procedure,.lpszClassName="CiukiFileProgress"};
  RegisterClassA(&cls);
  p->simple=!!(a->flags&0x100);
  p->window=CreateWindowExA(0,"CiukiFileProgress",p->simple && a->title?a->title:"File operation",0x80c80000u,80,80,400,126,a->window,0,0,p);
  if(!p->window){shell_free(p);return 0;}
  p->label=CreateWindowExA(0,"STATIC","Working...",0x50000000,12,12,370,32,p->window,0,0,0);
  CreateWindowExA(0,"BUTTON","Cancel",0x50000000,290,54,84,24,p->window,(HANDLE)2,0,0);
  ShowWindow(p->window,5);UpdateWindow(p->window);return p;
}
int shell_progress_tick(struct shell_progress *p,const char *file){
  if(!p)return 1;
  if(!p->simple)SetWindowTextA(p->label,file);
  MSG m;unsigned count=0;
  while(count++<64 && PeekMessageA(&m,0,0,0,1)){
    if(m.message==0x12){PostQuitMessage((int)m.wParam);p->cancelled=1;break;}
    TranslateMessage(&m);DispatchMessageA(&m);
  }
  return !p->cancelled;
}
void shell_progress_end(struct shell_progress *p){if(p){DestroyWindow(p->window);shell_free(p);}}
