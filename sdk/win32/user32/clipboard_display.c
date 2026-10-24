/* SPDX-License-Identifier: LGPL-2.1-or-later. Owner-display viewer requests.
 * HGLOBAL/PAINTSTRUCT/HDC are local at both ends; wire data are explicit. */
#include "internal.h"
extern HANDLE W32API GlobalAlloc(DWORD,DWORD);
extern HANDLE W32API GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
int u32_clip_display_send(HWND h,UINT m,WPARAM wp,LPARAM lp,LRESULT *result)
{
  if (m<WM_PAINTCLIPBOARD || m>WM_HSCROLLCLIPBOARD || m==0x30d || u32_window(h)) return 0;
  *result=0;
  if (h!=GetClipboardOwner()) { u32_fail(1400); return -1; }
  uint8_t data[24]={0}; DWORD n=8; PAINTSTRUCT paint={0};
  w32_put32(data,(DWORD)wp); w32_put32(data+4,(DWORD)lp);
  if (m==WM_SIZECLIPBOARD || m==WM_PAINTCLIPBOARD) {
    DWORD need=m==WM_SIZECLIPBOARD ? sizeof(RECT) : sizeof(PAINTSTRUCT);
    if (GlobalSize((HANDLE)lp)<need) { u32_fail(87); return -1; }
    void *p=GlobalLock((HANDLE)lp); if (!p) return -1;
    RECT r;
    if (m==WM_PAINTCLIPBOARD) { paint=*(PAINTSTRUCT *)p; r=paint.rcPaint; }
    else r=*(RECT *)p;
    GlobalUnlock((HANDLE)lp);
    w32_put32(data+8,r.left); w32_put32(data+12,r.top); w32_put32(data+16,r.right); w32_put32(data+20,r.bottom); n=24;
  }
  if (m==WM_ASKCBFORMATNAME && (!lp || !wp || wp>65536)) { u32_fail(87); return -1; }
  struct desk_clip_wire w={.op=DESK_CLIP_DISPLAY,.format=m,.bytes=n,.token=U32_PTR(h)},reply; void *out=0;
  if (!u32_clip_rpc(&w,data,&reply,&out)) return -1;
  if (reply.bytes<4 || !out) { HeapFree(GetProcessHeap(),0,out); u32_fail(13); return -1; }
  if (reply.bytes>=4 && out) {
    uint8_t *b=out; *result=(LONG)w32_u32(b);
    if (m==WM_ASKCBFORMATNAME && reply.bytes>4) {
      unsigned count=reply.bytes-4; if (count>wp) count=wp;
      w32_copy((void *)lp,b+4,count); ((char *)lp)[count-1]=0;
    } else if (m==WM_PAINTCLIPBOARD && reply.bytes>4) (void)u32_clip_display_blit(paint.hdc,b+4,reply.bytes-4,&paint.rcPaint);
  }
  HeapFree(GetProcessHeap(),0,out); return 1;
}
void u32_clip_display_event(const struct desk_packet *p)
{
  uint8_t input[24]={0}; unsigned n=p->clip.bytes; int error=0;
  if (n!=8 && n!=24) error=1;
  if (!error && p->clipboard_fd>=0) {
    struct ciuki_surface_info info={.size=sizeof(info)};
    if ((int)u32_backend.gate(CIUKI_SYS_SURFACE_INFO,p->clipboard_fd,U32_PTR(&info),0)<0 || n>info.allocation_bytes) error=1;
    else {
      DWORD address=u32_backend.gate(CIUKI_SYS_SURFACE_MAP,p->clipboard_fd,1,0);
      if ((int32_t)address<0) error=1;
      else { w32_copy(input,(void *)(uintptr_t)address,n); u32_backend.gate(CIUKI_SYS_MUNMAP,address,info.allocation_bytes,0); }
    }
  } else error=1;
  unsigned m=p->clip.format; HWND owner=GetClipboardOwner();
  if (!u32_window(owner)) error=1;
  HANDLE block=0; void *memory=0; uint8_t *output=0; DWORD bytes=4;
  struct u32_surface surface={.fd=-1}; HDC dc=0;
  LPARAM lp=(LONG)w32_u32(input+4); WPARAM wp=w32_u32(input);
  RECT r={(LONG)w32_u32(input+8),(LONG)w32_u32(input+12),(LONG)w32_u32(input+16),(LONG)w32_u32(input+20)};
  if (!error && m==WM_ASKCBFORMATNAME) {
    if (!wp || wp>65536) error=1;
    else { bytes=4+(DWORD)wp; output=HeapAlloc(GetProcessHeap(),8,bytes); if (!output) error=1; else lp=(LPARAM)(output+4); }
  } else if (!error && (m==WM_SIZECLIPBOARD || m==WM_PAINTCLIPBOARD)) {
    if (n!=24 || r.left<0 || r.top<0 || r.right<r.left || r.bottom<r.top || r.right>2048 || r.bottom>2048) error=1;
    if (!error && m==WM_PAINTCLIPBOARD) {
      if (!r.right || !r.bottom) error=1;
      else {
        bytes=44+(DWORD)r.right*(DWORD)r.bottom*4;
        if (bytes>DESK_CLIP_ITEM_MAX) error=1;
        else output=HeapAlloc(GetProcessHeap(),8,bytes);
        u32_gdi_ensure();
        if (!output || !u32_gdi) error=1;
        else {
          uint8_t *dib=output+4; w32_put32(dib,40); w32_put32(dib+4,r.right); w32_put32(dib+8,-r.bottom);
          w32_put16(dib+12,1); w32_put16(dib+14,32);
          surface=(struct u32_surface){dib+40,(DWORD)r.right,(DWORD)r.bottom,(DWORD)r.right*4,1,-1};
          dc=u32_gdi->acquire(&surface,(POINT){0,0},&r,1,0,0); if (!dc) error=1;
        }
      }
    }
    if (!error) {
      block=GlobalAlloc(2|0x40,m==WM_SIZECLIPBOARD ? sizeof(RECT) : sizeof(PAINTSTRUCT));
      memory=block ? GlobalLock(block) : 0;
      if (!memory) error=1;
      else {
        if (m==WM_SIZECLIPBOARD) *(RECT *)memory=r;
        else { PAINTSTRUCT *ps=memory; ps->hdc=dc; ps->rcPaint=r; ps->fErase=1; }
        GlobalUnlock(block); lp=(LPARAM)block;
      }
    }
  }
  LRESULT result=error ? 0 : SendMessageA(owner,m,wp,lp);
  if (dc) { RECT dirty[64]; (void)u32_gdi->release(dc,dirty,64); }
  if (block) GlobalFree(block);
  uint8_t fallback[4]={0};
  if (!output || error) { HeapFree(GetProcessHeap(),0,output); output=0; bytes=4; }
  w32_put32(output ? output : fallback,(DWORD)result);
  struct desk_clip_wire w={.op=DESK_CLIP_DISPLAY_REPLY,.format=m,.token=p->clip.token,
      .flags=p->clip.serial,.bytes=bytes},reply;
  (void)u32_clip_rpc(&w,output ? output : fallback,&reply,0);
  HeapFree(GetProcessHeap(),0,output);
}
