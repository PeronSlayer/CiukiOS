/* SPDX-License-Identifier: LGPL-2.1-or-later. Real USER/GDI clipboard objects. */
static void g5b_display_request(unsigned message,unsigned width,unsigned height)
{
  struct u32_surface s; CHECK(surface(6,1,&s));
  memset(s.pixels,0,24);
  w32_put32(s.pixels,message==WM_ASKCBFORMATNAME ? 64 : U32_PTR(windows[2]));
  if (message==WM_SIZECLIPBOARD || message==WM_PAINTCLIPBOARD) {
    w32_put32((uint8_t *)s.pixels+16,width); w32_put32((uint8_t *)s.pixels+20,height);
  }
  struct desk_packet p={.clipboard_fd=s.fd,.clip={.kind=DESK_CLIP_DISPLAY_EVENT,.op=DESK_CLIP_DISPLAY,
    .serial=501,.token=2,.format=message,.bytes=(message==WM_SIZECLIPBOARD || message==WM_PAINTCLIPBOARD) ? 24 : 8}};
  u32_clip_display_event(&p); drop(&s);
}
static void test_g5b_handles(void)
{
  struct { LONG mode,x,y; HANDLE metafile; } picture={8,640,480,0};
  DWORD (*create_palette)(const void *)=host_gdi_proc("CreatePalette");
  DWORD (*create_wmf)(DWORD,const void *)=host_gdi_proc("SetMetaFileBitsEx");
  DWORD (*create_emf)(DWORD,const void *)=host_gdi_proc("SetEnhMetaFileBits");
  unsigned (*palette_entries)(DWORD,unsigned,unsigned,void *)=host_gdi_proc("GetPaletteEntries");
  unsigned (*wmf_bits)(DWORD,unsigned,void *)=host_gdi_proc("GetMetaFileBitsEx");
  unsigned (*emf_bits)(DWORD,unsigned,void *)=host_gdi_proc("GetEnhMetaFileBits");
  CHECK(create_palette && create_wmf && create_emf && palette_entries);
  uint8_t palette[12]={0,3,2,0,255,0,0,0,0,255,0,0};
  uint8_t wmf[24]={0}; w32_put16(wmf,1); w32_put16(wmf+2,9); w32_put16(wmf+4,0x300);
  w32_put32(wmf+6,12); w32_put32(wmf+12,3); w32_put32(wmf+18,3);
  uint8_t emf[108]={0}; w32_put32(emf,1); w32_put32(emf+4,88); w32_put32(emf+40,0x464d4520);
  w32_put32(emf+44,0x10000); w32_put32(emf+48,108); w32_put32(emf+52,2);
  w32_put32(emf+88,14); w32_put32(emf+92,20);
  HANDLE pal=(HANDLE)(uintptr_t)create_palette(palette),enh=(HANDLE)(uintptr_t)create_emf(108,emf);
  picture.metafile=(HANDLE)(uintptr_t)create_wmf(24,wmf);
  HANDLE mfp=GlobalAlloc(2,sizeof(picture)); CHECK(pal && enh && picture.metafile && mfp);
  memcpy(GlobalLock(mfp),&picture,sizeof(picture)); GlobalUnlock(mfp);
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  CHECK(SetClipboardData(CF_PALETTE,pal)==pal && SetClipboardData(CF_METAFILEPICT,mfp)==mfp && SetClipboardData(CF_ENHMETAFILE,enh)==enh);
  CHECK(CloseClipboard());
  global_reuse_once=mfp;
  u32_clip_render_all(windows[0],0); /* owner exit frees its handles, retains bytes */
  CHECK(global_reuse_ready);
  CHECK(OpenClipboard(windows[0]));
  HANDLE got_pal=GetClipboardData(CF_PALETTE);
  CHECK(got_pal && got_pal!=pal);
  /* The owner has freed mfp. A reconstructed global block may have the same
   * address: unlike GDI object handles, host GlobalAlloc has no generation.
   * Force that legal reuse so this regression does not depend on ASLR/load. */
  HANDLE got_meta=GetClipboardData(CF_METAFILEPICT);
  CHECK(got_meta && got_meta==mfp && !global_reuse_once && !global_reuse_ready);
  HANDLE got_emf=GetClipboardData(CF_ENHMETAFILE);
  CHECK(got_emf && got_emf!=enh);
  uint8_t entries[8]; CHECK(palette_entries(U32_PTR(got_pal),0,2,entries)==2 && !memcmp(entries,palette+4,8));
  memcpy(&picture,GlobalLock(got_meta),sizeof(picture)); GlobalUnlock(got_meta);
  CHECK(picture.mode==8 && picture.x==640 && picture.y==480 && wmf_bits(U32_PTR(picture.metafile),0,0)==24 && emf_bits(U32_PTR(got_emf),0,0)==108);
  uint8_t got_wmf[24],got_enh[108];
  CHECK(wmf_bits(U32_PTR(picture.metafile),sizeof(got_wmf),got_wmf)==sizeof(got_wmf) && !memcmp(got_wmf,wmf,sizeof(wmf)));
  CHECK(emf_bits(U32_PTR(got_emf),sizeof(got_enh),got_enh)==sizeof(got_enh) && !memcmp(got_enh,emf,sizeof(emf)));
  CHECK(CloseClipboard() && palette_entries(U32_PTR(got_pal),0,0,0)==2); /* valid until Empty */
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard() && CloseClipboard());
  CHECK(!palette_entries(U32_PTR(got_pal),0,0,0) && !emf_bits(U32_PTR(got_emf),0,0));
  WCHAR upper[]={0x3a3,0x414,0xd801,0xdc00,0},lower[]={0x3c3,0x434,0xd801,0xdc28,0},name[16];
  UINT a=RegisterClipboardFormatW(upper),b=RegisterClipboardFormatW(lower);
  CHECK(a>=0xc000 && a==b && GetClipboardFormatNameW(a,name,16)==4 && !memcmp(name,upper,sizeof(upper)));
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  SetLastError(0); CHECK(!SetClipboardData(CF_TEXT,0) && !GetLastError());
  CHECK(!SetClipboardData(CF_LOCALE,0) && !GetLastError() && CloseClipboard());
  CHECK(OpenClipboard(windows[0])); HANDLE delayed_text=GetClipboardData(CF_UNICODETEXT); CHECK(delayed_text);
  WCHAR *text=GlobalLock(delayed_text); CHECK(text[0]=='l' && text[4]=='r' && !text[5] && g5b_locale_renders==1);
  CHECK(EmptyClipboard() && CloseClipboard());
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  uint32_t pixel=0x123456; g5b_bitmap=(HANDLE)(uintptr_t)host_color(1,1,&pixel); CHECK(g5b_bitmap);
  SetLastError(0); CHECK(!SetClipboardData(CF_BITMAP,0) && !GetLastError()); CHECK(CloseClipboard());
  CHECK(OpenClipboard(windows[0])); HANDLE got_bitmap=GetClipboardData(CF_BITMAP);
  CHECK(got_bitmap && g5b_bitmap_renders==1 && IsClipboardFormatAvailable(CF_DIB));
  CHECK(EmptyClipboard() && CloseClipboard()); g5b_bitmap=0;
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  SetLastError(0); CHECK(!SetClipboardData(CF_OWNERDISPLAY,0) && !GetLastError() && !u32_clip_has_delayed());
  CHECK(CloseClipboard() && IsClipboardFormatAvailable(CF_OWNERDISPLAY)); g5b_display=1;
  g5b_display_request(WM_ASKCBFORMATNAME,0,0); CHECK(display_bytes==68 && !strcmp((char *)display_output+4,"G5b owner"));
  g5b_display_request(WM_SIZECLIPBOARD,8,6); CHECK(g5b_display_sizes==1);
  g5b_display_request(WM_PAINTCLIPBOARD,8,6); CHECK(g5b_display_paints==1 && display_bytes==44+8*6*4);
  DWORD color=GetSysColor(5),pixel_color=((color&255)<<16)|(color&0xff00)|((color>>16)&255);
  CHECK(w32_u32((uint8_t *)display_output+44)==pixel_color);
  DWORD dc=0; uint32_t *pixels=NULL; DWORD bitmap=host_canvas(8,6,0x192837,&dc,&pixels); CHECK(bitmap);
  RECT partial={2,1,5,4}; CHECK(u32_clip_display_blit((HDC)(uintptr_t)dc,(uint8_t *)display_output+4,display_bytes-4,&partial));
  CHECK(pixels[0]==0x192837 && pixels[10]==pixel_color && pixels[47]==0x192837); host_canvas_free(dc,bitmap);
  g5b_display_request(WM_VSCROLLCLIPBOARD,0,0); g5b_display_request(WM_HSCROLLCLIPBOARD,0,0); CHECK(g5b_display_scrolls==2);
  g5b_display_request(WM_SIZECLIPBOARD,0,0); CHECK(g5b_display_sizes==2);
  free(display_output); display_output=0; display_bytes=0; g5b_display=0;
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard() && CloseClipboard());
  puts("TRACE clipboard retired GlobalAlloc address reused with exact reconstructed metafile bytes");
  puts("TRACE G5b clipboard GDI handle reconstruction/lifetimes, Unicode names, delayed CF_BITMAP, owner-display name/size/paint/scroll");
}
