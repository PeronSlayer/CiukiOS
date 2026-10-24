/* SPDX-License-Identifier: LGPL-2.1-or-later. Process-local GDI handles are
 * serialized to the desktop and reconstructed by each clipboard consumer. */
#include "internal.h"
extern HANDLE W32API GetModuleHandleA(const char *);
extern HANDLE W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(HANDLE,const char *);
extern HANDLE W32API GlobalAlloc(DWORD,DWORD);
extern HANDLE W32API GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
struct clip_mfp { LONG mode,x,y; HANDLE metafile; };
static struct {
  UINT(W32API *palette_entries)(DWORD,UINT,UINT,void *);
  DWORD(W32API *palette_create)(const void *);
  UINT(W32API *meta_bits)(DWORD,UINT,void *);
  DWORD(W32API *meta_create)(UINT,const void *);
  UINT(W32API *emf_bits)(DWORD,UINT,void *);
  DWORD(W32API *emf_create)(UINT,const void *);
  BOOL(W32API *delete_object)(DWORD);
  BOOL(W32API *delete_meta)(DWORD);
  BOOL(W32API *delete_emf)(DWORD);
} cg;
static BOOL clip_gdi(void)
{
  if (cg.delete_emf) return 1;
  HANDLE m=GetModuleHandleA("gdi32.dll"); if (!m) m=LoadLibraryA("gdi32.dll");
  if (!m) return 0;
#define LOAD(field,name) do { *(void **)(&cg.field)=GetProcAddress(m,name); if (!cg.field) return 0; } while (0)
  LOAD(palette_entries,"GetPaletteEntries"); LOAD(palette_create,"CreatePalette");
  LOAD(meta_bits,"GetMetaFileBitsEx"); LOAD(meta_create,"SetMetaFileBitsEx");
  LOAD(emf_bits,"GetEnhMetaFileBits"); LOAD(emf_create,"SetEnhMetaFileBits");
  LOAD(delete_object,"DeleteObject"); LOAD(delete_meta,"DeleteMetaFile"); LOAD(delete_emf,"DeleteEnhMetaFile");
#undef LOAD
  return 1;
}
BOOL u32_clip_handle_format(UINT f) { return f==3 || f==9 || f==14 || f==0x83 || f==0x8e; }
void u32_clip_handle_free(UINT f,HANDLE h)
{
  if (!h) return;
  if (f==2 || f==0x82) { if (u32_gdi && u32_gdi->bitmap) u32_gdi->bitmap(h,0,1); }
  else if (u32_clip_handle_format(f) && clip_gdi()) {
    if (f==9) cg.delete_object(U32_PTR(h));
    else if (f==14 || f==0x8e) cg.delete_emf(U32_PTR(h));
    else {
      struct clip_mfp *p=GlobalLock(h);
      if (p) { cg.delete_meta(U32_PTR(p->metafile)); GlobalUnlock(h); }
      GlobalFree(h);
    }
  } else GlobalFree(h);
}
void *u32_clip_handle_pack(UINT f,HANDLE h,DWORD *bytes)
{
  if (!clip_gdi()) { u32_fail(120); return 0; }
  DWORD prefix=0,n=0,object=U32_PTR(h); struct clip_mfp picture={0};
  if (f==3 || f==0x83) {
    if (GlobalSize(h)<sizeof(picture)) { u32_fail(87); return 0; }
    struct clip_mfp *p=GlobalLock(h); if (!p) return 0;
    picture=*p; GlobalUnlock(h); object=U32_PTR(picture.metafile); prefix=12;
  }
  if (f==9) { n=cg.palette_entries(object,0,0,0); if (!n || n>256) return 0; n*=4; prefix=4; }
  else n=(f==14 || f==0x8e) ? cg.emf_bits(object,0,0) : cg.meta_bits(object,0,0);
  if (!n || n>DESK_CLIP_ITEM_MAX-prefix) { u32_fail(87); return 0; }
  uint8_t *data=HeapAlloc(GetProcessHeap(),8,prefix+n); if (!data) return 0;
  BOOL ok;
  if (f==9) {
    w32_put16(data,0x300); w32_put16(data+2,n/4);
    ok=cg.palette_entries(object,0,n/4,data+4)==n/4;
  } else {
    if (prefix) { w32_put32(data,picture.mode); w32_put32(data+4,picture.x); w32_put32(data+8,picture.y); }
    ok=((f==14 || f==0x8e) ? cg.emf_bits(object,n,data+prefix) : cg.meta_bits(object,n,data+prefix))==n;
  }
  if (!ok) { HeapFree(GetProcessHeap(),0,data); return 0; }
  *bytes=prefix+n; return data;
}
HANDLE u32_clip_handle_unpack(UINT f,const uint8_t *data,DWORD n)
{
  if (!clip_gdi()) return 0;
  if (f==9) {
    if (n<4 || w32_u16(data)!=0x300 || !w32_u16(data+2) || w32_u16(data+2)>256 || n!=4u+4u*w32_u16(data+2)) return 0;
    return (HANDLE)(uintptr_t)cg.palette_create(data);
  }
  if (f==14 || f==0x8e) return (HANDLE)(uintptr_t)cg.emf_create(n,data);
  if (n<=12) return 0;
  DWORD meta=cg.meta_create(n-12,data+12); if (!meta) return 0;
  HANDLE h=GlobalAlloc(2|0x40,sizeof(struct clip_mfp));
  struct clip_mfp *p=h ? GlobalLock(h) : 0;
  if (!p) { if (h) GlobalFree(h); cg.delete_meta(meta); return 0; }
  *p=(struct clip_mfp){(LONG)w32_u32(data),(LONG)w32_u32(data+4),(LONG)w32_u32(data+8),(HANDLE)(uintptr_t)meta};
  GlobalUnlock(h); return h;
}
