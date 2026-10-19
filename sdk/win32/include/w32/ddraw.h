/* Original DirectDraw facade. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_DDRAW_H
#define CIUKI_W32_DDRAW_H
#include <w32/gdi32.h>
typedef int32_t DDRESULT;
typedef struct {
  DWORD a;
  uint16_t b, c;
  uint8_t d[8];
} DDGUID;
/* COM layout is independent of our private fields following the vtable. */
struct dd_interface {
  const void *const *vt;
  void *owner;
  DWORD generation, kind;
};
typedef struct dd_interface DD;
typedef struct {
  DWORD low, high;
} DDKEY;
typedef struct {
  DWORD size, flags, fourcc, bits, red, green, blue, alpha;
} DDPF;
#pragma pack(push, 4)
typedef struct {
  DWORD size, flags, height, width;
  LONG pitch;
  DWORD backbuffers, refresh, alpha, reserved;
  void *pixels;
  DDKEY destoverlay, destkey, srcoverlay, srckey;
  DDPF pixel;
  DWORD caps;
} DDDESC;
typedef struct {
  DDDESC old;
  DWORD caps2, caps3, caps4, texture_stage;
} DDDESC2;
typedef struct {
  DWORD size, effects, rop, dd_rop, rotation, z_op, z_low, z_high, z_base;
  DWORD z_dest_bits;
  uintptr_t z_dest;
  DWORD z_src_bits;
  uintptr_t z_src;
  DWORD alpha_edge_bits, alpha_edge, reserved, alpha_dest_bits;
  uintptr_t alpha_dest;
  DWORD alpha_src_bits;
  uintptr_t alpha_src, fill;
  DDKEY destkey, srckey;
} DDFX;
typedef struct {
  char driver[512], description[512];
  uint64_t version;
  DWORD vendor, device, subsystem, revision;
  DDGUID guid;
} DDIDENT;
typedef struct {
  DDIDENT old;
  DWORD whql, padding;
} DDIDENT2;
#pragma pack(pop)
typedef struct {
  uint16_t red[256], green[256], blue[256];
} DDGAMMA;
typedef struct {
  DWORD size, flags;
  LONG brightness, contrast, hue, saturation, sharpness, gamma, enable;
  DWORD reserved;
} DDCOLOR;
#ifndef W32_HOST_MODEL
_Static_assert(sizeof(DDDESC) == 108 && sizeof(DDDESC2) == 124,
               "DX1/DX7 surface descriptors");
_Static_assert(sizeof(DDFX) == 100 && sizeof(DDPF) == 32,
               "DX7 blit/pixel layouts");
_Static_assert(sizeof(DDIDENT) == 1064 && sizeof(DDIDENT2) == 1072,
               "DX6/DX7 device identifiers");
#endif
#define DD_OK 0
#define DDERR(n) ((DDRESULT)(0x88760000u | (n)))
#define DDERR_INVALIDPARAMS ((DDRESULT)0x80070057u)
#define DDERR_OUTOFMEMORY ((DDRESULT)0x8007000eu)
#define DDERR_UNSUPPORTED ((DDRESULT)0x80004001u)
#define DDERR_INVALIDCAPS DDERR(100)
#define DDERR_INVALIDMODE DDERR(120)
#define DDERR_INVALIDPIXELFORMAT DDERR(145)
#define DDERR_INVALIDRECT DDERR(150)
#define DDERR_NOCLIPLIST DDERR(205)
#define DDERR_NOCOOPERATIVELEVELSET DDERR(212)
#define DDERR_NOCOLORKEY DDERR(215)
#define DDERR_NOEXCLUSIVEMODE DDERR(225)
#define DDERR_NOTFOUND DDERR(255)
#define DDERR_NOOVERLAYHW DDERR(260)
#define DDERR_OUTOFVIDEOMEMORY DDERR(380)
#define DDERR_SURFACEBUSY DDERR(430)
#define DDERR_SURFACELOST DDERR(450)
#define DDERR_WASSTILLDRAWING DDERR(540)
#define DDERR_WRONGMODE DDERR(587)
#define DDERR_EXCLUSIVEMODEALREADYSET DDERR(581)
#define DDERR_PRIMARYSURFACEALREADYEXISTS DDERR(564)
#define DDERR_NOTFLIPPABLE DDERR(582)
#define DDERR_NOTLOCKED DDERR(584)
#define DDERR_DCALREADYCREATED DDERR(620)
#define DDERR_NOPALETTEATTACHED DDERR(572)
#define DDERR_NOCLIPPERATTACHED DDERR(568)
#define DDERR_CLIPPERISUSINGHWND DDERR(567)
#define DDERR_IMPLICITLYCREATED DDERR(588)
#define DDERR_MOREDATA DDERR(690)
#define DDERR_EXPIRED DDERR(691)
#define DDSD_CAPS 1u
#define DDSD_HEIGHT 2u
#define DDSD_WIDTH 4u
#define DDSD_PITCH 8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_REFRESHRATE 0x40000u
#define DDSCAPS_BACKBUFFER 4u
#define DDSCAPS_COMPLEX 8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_OVERLAY 0x80u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_TEXTURE 0x1000u
#define DDSCAPS_3DDEVICE 0x2000u
#define DDSCAPS_VIDEOMEMORY 0x4000u
#define DDSCAPS_ZBUFFER 0x20000u
#define DDSCAPS_LOCALVIDMEM 0x10000000u
#define DDSCL_FULLSCREEN 1u
#define DDSCL_ALLOWREBOOT 2u
#define DDSCL_NOWINDOWCHANGES 4u
#define DDSCL_NORMAL 8u
#define DDSCL_EXCLUSIVE 0x10u
#define DDSCL_ALLOWMODEX 0x40u
#define DDSCL_FPUSETUP 0x800u
#define DDSCL_FPUPRESERVE 0x1000u
#define DDLOCK_WAIT 1u
#define DDLOCK_READONLY 0x10u
#define DDLOCK_DONOTWAIT 0x4000u
#define DDFLIP_WAIT 1u
#define DDFLIP_NOVSYNC 8u
#define DDFLIP_DONOTWAIT 0x20u
#define DDFLIP_INTERVAL2 0x02000000u
#define DDFLIP_INTERVAL3 0x03000000u
#define DDFLIP_INTERVAL4 0x04000000u
#define DDBLT_COLORFILL 0x400u
#define DDBLT_DDFX 0x800u
#define DDBLT_KEYDEST 0x2000u
#define DDBLT_KEYDESTOVERRIDE 0x4000u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u
#define DDBLT_ROP 0x20000u
#define DDBLT_WAIT 0x1000000u
#define DDBLT_DONOTWAIT 0x8000000u
#define DDBLTFX_MIRRORLEFTRIGHT 2u
#define DDBLTFX_MIRRORUPDOWN 4u
#define DDBLTFAST_SRCCOLORKEY 1u
#define DDBLTFAST_DESTCOLORKEY 2u
#define DDBLTFAST_WAIT 0x10u
#define DDBLTFAST_DONOTWAIT 0x20u
#define DDCKEY_COLORSPACE 1u
#define DDCKEY_DESTBLT 2u
#define DDCKEY_SRCBLT 8u
#define DDPCAPS_8BIT 4u
#define DDPCAPS_ALLOW256 0x40u
#define DDPF_PALETTEINDEXED8 0x20u
#define DDPF_RGB 0x40u
#define DDSPD_IUNKNOWNPOINTER 1u
#define DDSPD_VOLATILE 2u
#define DDEDM_REFRESHRATES 1u
extern const DDGUID dd_iids[4], dd_surface_iids[5], dd_unknown_iid,
    dd_palette_iid, dd_clipper_iid, dd_gamma_iid, dd_color_iid;
DDRESULT W32API DirectDrawCreate(const DDGUID *, DD **, void *);
DDRESULT W32API DirectDrawCreateEx(const DDGUID *, void **, const DDGUID *,
                                   void *);
DDRESULT W32API DirectDrawCreateClipper(DWORD, DD **, void *);
typedef BOOL(W32API *dd_enum_a)(DDGUID *, char *, char *, void *);
typedef BOOL(W32API *dd_enum_w)(DDGUID *, WCHAR *, WCHAR *, void *);
typedef BOOL(W32API *dd_enum_exa)(DDGUID *, char *, char *, void *, void *);
typedef BOOL(W32API *dd_enum_exw)(DDGUID *, WCHAR *, WCHAR *, void *, void *);
DDRESULT W32API DirectDrawEnumerateA(dd_enum_a, void *);
DDRESULT W32API DirectDrawEnumerateW(dd_enum_w, void *);
DDRESULT W32API DirectDrawEnumerateExA(dd_enum_exa, void *, DWORD);
DDRESULT W32API DirectDrawEnumerateExW(dd_enum_exw, void *, DWORD);
/* Transport adapter: no authority-bearing descriptor or framebuffer pointer.
 * lease/poll return active=1 only with a current token and generation.
 * clip returns screen-space rectangles; window publishes with USER/GDI.
 * Lifetime: install before creating objects, callbacks outlive all objects. */
struct dd_mode {
  DWORD width, height, bpp;
};
struct dd_display {
  DWORD width, height, bpp, vram, count;
  struct dd_mode modes[96];
};
struct dd_lease {
  DWORD active, token, generation, width, height, bpp, pitch;
};
struct dd_backend {
  BOOL (*display)(struct dd_display *);
  DDRESULT (*lease)(uintptr_t, DWORD, DWORD, DWORD, struct dd_lease *);
  void (*release)(uintptr_t);
  void (*poll)(uintptr_t, struct dd_lease *);
  DDRESULT (*frame)(const struct dd_lease *, const struct w32_dib *);
  DDRESULT (*palette)(const struct dd_lease *, DWORD, DWORD, const DWORD *);
  DDRESULT (*gamma)(const struct dd_lease *, const DDGAMMA *);
  DDRESULT (*clip)(uintptr_t, RECT *, DWORD *);
  DDRESULT (*window)(uintptr_t, const struct w32_dib *, const RECT *, DWORD);
  uint64_t (*clock_ns)(void);
  void (*wait_ns)(uint64_t);
};
void W32API w32_ddraw_set_backend(const struct dd_backend *);
void W32API w32_ddraw_process_exit(void);
DWORD W32API w32_ddraw_surface_bytes(void);
void W32API w32_ddraw_tick(void);
#endif
