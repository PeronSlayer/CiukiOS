/* Original scalar GDI engine. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_W32_GDI32_H
#define CIUKI_W32_GDI32_H
#include "base.h"
typedef int32_t BOOL;
typedef uint32_t DWORD, COLORREF, HGDIOBJ, HDC, HBITMAP, HBRUSH, HPEN, HRGN,
    HPALETTE, HFONT;
typedef struct {
  LONG x, y;
} POINT;
typedef struct {
  LONG cx, cy;
} SIZE;
typedef struct {
  LONG left, top, right, bottom;
} RECT;
typedef struct {
  uint8_t rgbBlue, rgbGreen, rgbRed, rgbReserved;
} RGBQUAD;
typedef struct {
  DWORD biSize;
  LONG biWidth, biHeight;
  uint16_t biPlanes, biBitCount;
  DWORD biCompression, biSizeImage;
  LONG biXPelsPerMeter, biYPelsPerMeter;
  DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct {
  BITMAPINFOHEADER bmiHeader;
  RGBQUAD bmiColors[1];
} BITMAPINFO;
typedef struct {
  LONG bmType, bmWidth, bmHeight, bmWidthBytes;
  uint16_t bmPlanes, bmBitsPixel;
  void *bmBits;
} BITMAP;
typedef struct {
  BITMAP dsBm;
  BITMAPINFOHEADER dsBmih;
  DWORD dsBitfields[3];
  void *dshSection;
  DWORD dsOffset;
} DIBSECTION;
typedef struct {
  uint8_t peRed, peGreen, peBlue, peFlags;
} PALETTEENTRY;
typedef struct {
  uint16_t palVersion, palNumEntries;
  PALETTEENTRY palPalEntry[1];
} LOGPALETTE;
typedef struct {
  DWORD lbStyle;
  COLORREF lbColor;
  uintptr_t lbHatch;
} LOGBRUSH;
typedef struct {
  DWORD lopnStyle;
  POINT lopnWidth;
  COLORREF lopnColor;
} LOGPEN;
typedef struct {
  DWORD dwSize, iType, nCount, nRgnSize;
  RECT rcBound;
} RGNDATAHEADER;
typedef struct {
  RGNDATAHEADER rdh;
  char Buffer[1];
} RGNDATA;
typedef struct {
  DWORD cbSize;
  int iTabLength, iLeftMargin, iRightMargin;
  DWORD uiLengthDrawn;
} DRAWTEXTPARAMS;
#define RGB(r, g, b)                                                           \
  ((COLORREF)((uint8_t)(r) | ((DWORD)(uint8_t)(g) << 8) |                      \
              ((DWORD)(uint8_t)(b) << 16)))
#define CLR_INVALID 0xffffffffu
#define PALETTEINDEX(i) (0x01000000u | (uint16_t)(i))
#define PALETTERGB(r, g, b) (0x02000000u | RGB(r, g, b))
#define TRUE 1
#define FALSE 0
#define TA_LEFT 0
#define TA_TOP 0
#define TA_NOUPDATECP 0
#define DT_LEFT 0
#define DT_TOP 0
#define HS_HORIZONTAL 0
#define HS_VERTICAL 1
#define HS_FDIAGONAL 2
#define HS_BDIAGONAL 3
#define HS_CROSS 4
#define HS_DIAGCROSS 5
#define R2_BLACK 1
#define R2_NOTMERGEPEN 2
#define R2_MASKNOTPEN 3
#define R2_NOTCOPYPEN 4
#define R2_MASKPENNOT 5
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOTMASKPEN 8
#define R2_MASKPEN 9
#define R2_NOTXORPEN 10
#define R2_NOP 11
#define R2_MERGENOTPEN 12
#define R2_COPYPEN 13
#define R2_MERGEPENNOT 14
#define R2_MERGEPEN 15
#define R2_WHITE 16
#define OBJ_PEN 1
#define OBJ_BRUSH 2
#define OBJ_DC 3
#define OBJ_PAL 5
#define OBJ_FONT 6
#define OBJ_BITMAP 7
#define OBJ_REGION 8
#define OBJ_MEMDC 10
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define PLANES 14
#define RASTERCAPS 38
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define SIZEPALETTE 104
#define NUMRESERVED 106
#define FLOODFILLBORDER 0
#define FLOODFILLSURFACE 1
#define BI_RGB 0
#define BI_BITFIELDS 3
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#define CBM_INIT 4
#define SRCCOPY 0x00cc0020u
#define SRCPAINT 0x00ee0086u
#define SRCAND 0x008800c6u
#define SRCINVERT 0x00660046u
#define PATCOPY 0x00f00021u
#define PATINVERT 0x005a0049u
#define DSTINVERT 0x00550009u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00ff0062u
#define BLACKONWHITE 1
#define WHITEONBLACK 2
#define COLORONCOLOR 3
#define HALFTONE 4
#define TRANSPARENT 1
#define OPAQUE 2
#define MM_TEXT 1
#define MM_LOMETRIC 2
#define MM_HIMETRIC 3
#define MM_LOENGLISH 4
#define MM_HIENGLISH 5
#define MM_TWIPS 6
#define MM_ISOTROPIC 7
#define MM_ANISOTROPIC 8
#define RGN_AND 1
#define RGN_OR 2
#define RGN_XOR 3
#define RGN_DIFF 4
#define RGN_COPY 5
#define ERROR 0
#define NULLREGION 1
#define SIMPLEREGION 2
#define COMPLEXREGION 3
#define ALTERNATE 1
#define WINDING 2
#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_DASHDOT 3
#define PS_DASHDOTDOT 4
#define PS_NULL 5
#define PS_INSIDEFRAME 6
#define BS_SOLID 0
#define BS_NULL 1
#define BS_HATCHED 2
#define BS_PATTERN 3
#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define OEM_FIXED_FONT 10
#define ANSI_FIXED_FONT 11
#define ANSI_VAR_FONT 12
#define SYSTEM_FONT 13
#define DEVICE_DEFAULT_FONT 14
#define DEFAULT_PALETTE 15
#define SYSTEM_FIXED_FONT 16
#define DEFAULT_GUI_FONT 17
#define DC_BRUSH 18
#define DC_PEN 19
#define PC_RESERVED 1
#define PC_EXPLICIT 2
#define PC_NOCOLLAPSE 4
#define TA_UPDATECP 1
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define ETO_OPAQUE 2
#define ETO_CLIPPED 4
#define ETO_GLYPH_INDEX 16
#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 16
#define DT_SINGLELINE 32
#define DT_EXPANDTABS 64
#define DT_TABSTOP 128
#define DT_NOCLIP 256
#define DT_EXTERNALLEADING 512
#define DT_CALCRECT 1024
#define DT_NOPREFIX 2048
#define DT_INTERNAL 4096
#define DT_EDITCONTROL 8192
#define DT_PATH_ELLIPSIS 16384
#define DT_END_ELLIPSIS 32768
#define DT_MODIFYSTRING 65536
#define DT_RTLREADING 131072
#define DT_WORD_ELLIPSIS 262144
#define DT_HIDEPREFIX 1048576
#define DT_PREFIXONLY 2097152

/* Borrowed buffer; palette entries are 0x00RRGGBB (COLORREF is 0x00BBGGRR).
 * Dimensions <=8192; pitch includes padding, size must cover the last row.
 * 16-bit BI_RGB is 555; pass masks for 565. bits addresses storage row zero. */
struct w32_dib {
  uint8_t *bits;
  DWORD width, height, pitch, size, bpp, top_down;
  DWORD masks[3], colors[256], color_count;
};
BOOL w32_dib_validate(const struct w32_dib *);
DWORD w32_dib_raw(const struct w32_dib *, int, int);
DWORD w32_dib_rgb(const struct w32_dib *, int, int);
DWORD w32_dib_encode(const struct w32_dib *, DWORD);
void w32_dib_put(struct w32_dib *, int, int, DWORD);
DWORD w32_gdi_rop3(unsigned, DWORD, DWORD, DWORD);
/* device coordinates, signed extents; clip called only in destination bounds.
 * pattern may be NULL (solid 0xRRGGBB); transparent <0 means no colour key.
 * Returns FALSE before drawing on invalid layouts/allocation failure. */
BOOL w32_dib_blt(struct w32_dib *, int, int, int, int, const struct w32_dib *,
                 int, int, int, int, unsigned, unsigned, const struct w32_dib *,
                 POINT, DWORD, int64_t, int (*)(void *, int, int), void *);

/* USER32/DirectDraw seams. origin is the DC's (0,0) in the backing buffer;
 * LPtoDP/DPtoLP remain relative to that DC. Visible regions and damage use
 * buffer coordinates. All-zero RGB masks select BI_RGB's 555/BGR defaults.
 * Borrowed surface stays alive through DeleteDC or
 * successful rebind. Visible regions are copied and are independent of SaveDC.
 * Damage is in buffer device pixels. Callbacks must not delete the active DC.
 */
HDC W32API w32_gdi_surface_dc(const struct w32_dib *, POINT,
                              void (*)(void *, const RECT *), void *);
BOOL W32API w32_gdi_bind_surface(HDC, const struct w32_dib *);
/* Absolute screen origin is separate from the buffer offset. */
BOOL W32API w32_gdi_set_screen_origin(HDC, POINT);
BOOL W32API w32_gdi_set_visible_region(HDC, HRGN);
BOOL W32API w32_gdi_set_desktop(const struct w32_dib *);
BOOL W32API w32_gdi_window_shape(HDC, HRGN);
void W32API w32_gdi_palette_hook(void (*)(void *, HDC, unsigned), void *);
void W32API w32_gdi_process_exit(void);
DWORD W32API w32_gdi_live_objects(void);
DWORD W32API w32_gdi_surface_bytes(void);

/* f5-10C provides faces; no LOGFONT policy is imposed here. Metrics/advances
 * are signed 26.6 pixels at the selected size. raster returns owned coverage.
 * retain/release balance the HFONT lifetime, including saved DC references. */
struct w32_gdi_glyph {
  DWORD width, height, pitch;
  int left, top, advance;
  uint8_t *coverage;
};
struct w32_gdi_font_metrics {
  int ascent, descent, line_gap, max_advance;
};
struct w32_gdi_font_provider {
  int (*metrics)(void *, unsigned, struct w32_gdi_font_metrics *);
  int (*raster)(void *, unsigned, DWORD, int, struct w32_gdi_glyph *);
  void (*free_glyph)(void *, struct w32_gdi_glyph *);
  void (*retain)(void *);
  void (*release)(void *);
  int (*kerning)(void *, unsigned, DWORD, DWORD, int *);
  /* f5-10C supplies LOGFONT metadata for GetObject(A/W), or NULL. */
  int (*get_object)(void *, int, void *, int);
};
HFONT W32API w32_gdi_create_font(const struct w32_gdi_font_provider *, void *,
                                 unsigned);
BOOL W32API w32_gdi_stock_font(const struct w32_gdi_font_provider *, void *,
                               unsigned);
int W32API w32_gdi_draw_text(HDC, const void *, int, RECT *, unsigned, int,
                             DRAWTEXTPARAMS *);
BOOL W32API w32_gdi_draw_icon(HDC, int, int, int, int, HBITMAP, HBITMAP);

HDC W32API CreateCompatibleDC(HDC);
HDC W32API CreateDCA(const char *, const char *, const char *, const void *);
HDC W32API CreateDCW(const WCHAR *, const WCHAR *, const WCHAR *, const void *);
BOOL W32API DeleteDC(HDC);
int W32API SaveDC(HDC);
BOOL W32API RestoreDC(HDC, int);
HGDIOBJ W32API SelectObject(HDC, HGDIOBJ);
HGDIOBJ W32API GetStockObject(int);
HGDIOBJ W32API GetCurrentObject(HDC, unsigned);
BOOL W32API DeleteObject(HGDIOBJ);
DWORD W32API GetObjectType(HGDIOBJ);
int W32API GetObjectA(HGDIOBJ, int, void *);
int W32API GetObjectW(HGDIOBJ, int, void *);
int W32API GetDeviceCaps(HDC, int);
BOOL W32API GetDCOrgEx(HDC, POINT *);
COLORREF W32API SetBkColor(HDC, COLORREF);
COLORREF W32API GetBkColor(HDC);
COLORREF W32API SetTextColor(HDC, COLORREF);
COLORREF W32API GetTextColor(HDC);
COLORREF W32API SetDCBrushColor(HDC, COLORREF);
COLORREF W32API GetDCBrushColor(HDC);
COLORREF W32API SetDCPenColor(HDC, COLORREF);
COLORREF W32API GetDCPenColor(HDC);
int W32API SetBkMode(HDC, int);
int W32API GetBkMode(HDC);
int W32API SetROP2(HDC, int);
int W32API GetROP2(HDC);
int W32API SetStretchBltMode(HDC, int);
int W32API GetStretchBltMode(HDC);
int W32API SetPolyFillMode(HDC, int);
int W32API GetPolyFillMode(HDC);
int W32API SetMapMode(HDC, int);
int W32API GetMapMode(HDC);
BOOL W32API SetViewportOrgEx(HDC, int, int, POINT *);
BOOL W32API SetWindowOrgEx(HDC, int, int, POINT *);
BOOL W32API SetViewportExtEx(HDC, int, int, SIZE *);
BOOL W32API SetWindowExtEx(HDC, int, int, SIZE *);
BOOL W32API GetViewportOrgEx(HDC, POINT *);
BOOL W32API GetWindowOrgEx(HDC, POINT *);
BOOL W32API GetViewportExtEx(HDC, SIZE *);
BOOL W32API GetWindowExtEx(HDC, SIZE *);
BOOL W32API SetBrushOrgEx(HDC, int, int, POINT *);
BOOL W32API GetBrushOrgEx(HDC, POINT *);
BOOL W32API LPtoDP(HDC, POINT *, int);
BOOL W32API DPtoLP(HDC, POINT *, int);
HBITMAP W32API CreateDIBSection(HDC, const BITMAPINFO *, unsigned, void **,
                                void *, DWORD);
HBITMAP W32API CreateBitmap(int, int, unsigned, unsigned, const void *);
HBITMAP W32API CreateBitmapIndirect(const BITMAP *);
HBITMAP W32API CreateCompatibleBitmap(HDC, int, int);
HBITMAP W32API CreateDIBitmap(HDC, const BITMAPINFOHEADER *, DWORD,
                              const void *, const BITMAPINFO *, unsigned);
int W32API GetDIBits(HDC, HBITMAP, unsigned, unsigned, void *, BITMAPINFO *,
                     unsigned);
int W32API SetDIBits(HDC, HBITMAP, unsigned, unsigned, const void *,
                     const BITMAPINFO *, unsigned);
int W32API SetDIBitsToDevice(HDC, int, int, DWORD, DWORD, int, int, unsigned,
                             unsigned, const void *, const BITMAPINFO *,
                             unsigned);
int W32API StretchDIBits(HDC, int, int, int, int, int, int, int, int,
                         const void *, const BITMAPINFO *, unsigned, DWORD);
BOOL W32API BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
BOOL W32API StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
BOOL W32API PatBlt(HDC, int, int, int, int, DWORD);
BOOL W32API MaskBlt(HDC, int, int, int, int, HDC, int, int, HBITMAP, int, int,
                    DWORD);
BOOL W32API TransparentBlt(HDC, int, int, int, int, HDC, int, int, int, int,
                           COLORREF);
BOOL W32API GdiFlush(void);
COLORREF W32API GetPixel(HDC, int, int);
COLORREF W32API SetPixel(HDC, int, int, COLORREF);
BOOL W32API SetPixelV(HDC, int, int, COLORREF);
BOOL W32API ExtFloodFill(HDC, int, int, COLORREF, unsigned);
BOOL W32API MoveToEx(HDC, int, int, POINT *);
BOOL W32API GetCurrentPositionEx(HDC, POINT *);
BOOL W32API LineTo(HDC, int, int);
BOOL W32API Polyline(HDC, const POINT *, int);
BOOL W32API PolyBezier(HDC, const POINT *, DWORD);
BOOL W32API Arc(HDC, int, int, int, int, int, int, int, int);
BOOL W32API Rectangle(HDC, int, int, int, int);
BOOL W32API Ellipse(HDC, int, int, int, int);
BOOL W32API RoundRect(HDC, int, int, int, int, int, int);
BOOL W32API Polygon(HDC, const POINT *, int);
BOOL W32API PolyPolygon(HDC, const POINT *, const int *, int);
int W32API FillRect(HDC, const RECT *, HBRUSH);
int W32API FrameRect(HDC, const RECT *, HBRUSH);
BOOL W32API InvertRect(HDC, const RECT *);
HPEN W32API CreatePen(int, int, COLORREF);
HPEN W32API CreatePenIndirect(const LOGPEN *);
HBRUSH W32API CreateSolidBrush(COLORREF);
HBRUSH W32API CreateHatchBrush(int, COLORREF);
HBRUSH W32API CreatePatternBrush(HBITMAP);
HBRUSH W32API CreateBrushIndirect(const LOGBRUSH *);
HRGN W32API CreateRectRgn(int, int, int, int);
HRGN W32API CreateRectRgnIndirect(const RECT *);
BOOL W32API SetRectRgn(HRGN, int, int, int, int);
HRGN W32API CreateEllipticRgn(int, int, int, int);
HRGN W32API CreateEllipticRgnIndirect(const RECT *);
HRGN W32API CreatePolygonRgn(const POINT *, int, int);
HRGN W32API CreatePolyPolygonRgn(const POINT *, const int *, int, int);
int W32API CombineRgn(HRGN, HRGN, HRGN, int);
int W32API OffsetRgn(HRGN, int, int);
BOOL W32API PtInRegion(HRGN, int, int);
BOOL W32API RectInRegion(HRGN, const RECT *);
BOOL W32API EqualRgn(HRGN, HRGN);
int W32API GetRgnBox(HRGN, RECT *);
DWORD W32API GetRegionData(HRGN, DWORD, RGNDATA *);
HRGN W32API ExtCreateRegion(const void *, DWORD, const RGNDATA *);
BOOL W32API FillRgn(HDC, HRGN, HBRUSH);
BOOL W32API FrameRgn(HDC, HRGN, HBRUSH, int, int);
BOOL W32API InvertRgn(HDC, HRGN);
BOOL W32API PaintRgn(HDC, HRGN);
int W32API SelectClipRgn(HDC, HRGN);
int W32API ExtSelectClipRgn(HDC, HRGN, int);
int W32API IntersectClipRect(HDC, int, int, int, int);
int W32API ExcludeClipRect(HDC, int, int, int, int);
int W32API OffsetClipRgn(HDC, int, int);
int W32API GetClipBox(HDC, RECT *);
int W32API GetClipRgn(HDC, HRGN);
BOOL W32API PtVisible(HDC, int, int);
BOOL W32API RectVisible(HDC, const RECT *);
HPALETTE W32API CreatePalette(const LOGPALETTE *);
HPALETTE W32API SelectPalette(HDC, HPALETTE, BOOL);
unsigned W32API RealizePalette(HDC);
BOOL W32API AnimatePalette(HPALETTE, unsigned, unsigned, const PALETTEENTRY *);
unsigned W32API SetPaletteEntries(HPALETTE, unsigned, unsigned,
                                  const PALETTEENTRY *);
unsigned W32API GetPaletteEntries(HPALETTE, unsigned, unsigned, PALETTEENTRY *);
unsigned W32API GetSystemPaletteEntries(HDC, unsigned, unsigned,
                                        PALETTEENTRY *);
unsigned W32API GetNearestPaletteIndex(HPALETTE, COLORREF);
COLORREF W32API GetNearestColor(HDC, COLORREF);
BOOL W32API UnrealizeObject(HGDIOBJ);
unsigned W32API SetDIBColorTable(HDC, unsigned, unsigned, const RGBQUAD *);
unsigned W32API GetDIBColorTable(HDC, unsigned, unsigned, RGBQUAD *);
BOOL W32API TextOutA(HDC, int, int, const char *, int);
BOOL W32API TextOutW(HDC, int, int, const WCHAR *, int);
BOOL W32API ExtTextOutA(HDC, int, int, unsigned, const RECT *, const char *,
                        unsigned, const int *);
BOOL W32API ExtTextOutW(HDC, int, int, unsigned, const RECT *, const WCHAR *,
                        unsigned, const int *);
BOOL W32API GetTextExtentPoint32A(HDC, const char *, int, SIZE *);
BOOL W32API GetTextExtentPoint32W(HDC, const WCHAR *, int, SIZE *);
unsigned W32API SetTextAlign(HDC, unsigned);
unsigned W32API GetTextAlign(HDC);
int W32API SetTextCharacterExtra(HDC, int);
int W32API GetTextCharacterExtra(HDC);
HDC W32API CreateICA(const char *, const char *, const char *, const void *);
HDC W32API CreateICW(const WCHAR *, const WCHAR *, const WCHAR *, const void *);
BOOL W32API OffsetViewportOrgEx(HDC, int, int, POINT *);
BOOL W32API OffsetWindowOrgEx(HDC, int, int, POINT *);
BOOL W32API ScaleViewportExtEx(HDC, int, int, int, int, SIZE *);
BOOL W32API ScaleWindowExtEx(HDC, int, int, int, int, SIZE *);
BOOL W32API ResizePalette(HPALETTE, unsigned);
unsigned W32API GetSystemPaletteUse(HDC);
unsigned W32API SetSystemPaletteUse(HDC, unsigned);
DWORD W32API GdiSetBatchLimit(DWORD);
BOOL W32API GetTextExtentPointA(HDC, const char *, int, SIZE *);
BOOL W32API GetTextExtentPointW(HDC, const WCHAR *, int, SIZE *);
BOOL W32API GetTextExtentExPointA(HDC, const char *, int, int, int *, int *,
                                  SIZE *);
BOOL W32API GetTextExtentExPointW(HDC, const WCHAR *, int, int, int *, int *,
                                  SIZE *);
#endif
