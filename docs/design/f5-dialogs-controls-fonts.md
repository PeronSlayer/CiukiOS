# F5-10C implementation decisions and research

Research before implementation, 2026-10-19. The existing USER core owns HWNDs,
queues, focus, surfaces and destruction. The existing GDI engine owns DCs,
clipping, drawing and coverage composition; its provider boundary explicitly
leaves selection policy to this change. No Wine implementation is imported.

Primary references consulted:

- Microsoft [DLGTEMPLATEEX](https://learn.microsoft.com/en-us/windows/win32/dlgbox/dlgtemplateex)
  and [DLGITEMTEMPLATEEX](https://learn.microsoft.com/en-us/windows/win32/dlgbox/dlgitemtemplateex):
  little-endian fields, UTF-16 string/ordinal arrays and DWORD control alignment.
- Microsoft [dialog creation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-dialogboxindirectparama)
  and [IsDialogMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-isdialogmessagea):
  initialization, owner disabling, modal dispatch and control key requests.
- Microsoft [GetDialogBaseUnits](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdialogbaseunits)
  and [dialog average width](https://devblogs.microsoft.com/oldnewthing/20221103-00/?p=107350/):
  horizontal units divide by four, vertical by eight; alphabet extent, rather
  than tmAveCharWidth, determines dialog width. The directive supplies the
  qualification targets 8x16 (System) and 6x13 (MS Sans Serif 8 pt/96 dpi).
  Those exact numbers are directive reference values, not measured Windows
  captures or values promised for all machines by the API documentation.
- Microsoft [font creation](https://learn.microsoft.com/en-us/windows/win32/gdi/font-creation-and-selection)
  and historical [KB Q82754](https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/82/754.HTM):
  logical/physical mapping and scalable family fallback. Q82754 describes 3.1,
  not an experimentally established Windows 98 penalty table. An exact 98
  penalty order remains a qualification gap; do not claim it from this source.
- R. Gery, [Windows Font Mapping](https://learn.microsoft.com/en-us/previous-versions/ms969909(v=msdn.10)),
  MSDN 1992: the published core-mapper penalty weights (table 1), the
  substitution penalty, don't-care rules and GDI simulations. The selection
  layer uses these weights as the documented approximation of the Windows 98
  mapper.
- FreeType [driver properties](https://freetype.org/freetype2/docs/reference/ft2-properties.html)
  and [glyph retrieval](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html):
  v35 is the Windows 98-era rasterizer choice; grey glyph metrics and advances
  are size dependent. Use the pinned SDK 2.13.3 source/configuration and FTL.
- Microsoft [OpenType file format](https://learn.microsoft.com/en-us/typography/opentype/spec/otff):
  big-endian tables; validate directory and table ranges before FreeType.
- Liberation [2.1.5 upstream](https://github.com/liberationfonts/liberation-fonts/tree/2.1.5):
  preserve the approved immutable faces and OFL notices, without Microsoft files.
- Microsoft [AddFontResource](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-addfontresourcea),
  [OPENFILENAME](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamea),
  [TrackPopupMenuEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex)
  and [MessageBox](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-messageboxa):
  resource references/notifications, buffer bounds, popup result versus command
  delivery and button/default-button sets.

The current Microsoft references include later Windows policy. Modern DPI,
UIPI and foreground-lock rules are not adopted. The local Windows 98 Resource
Kit chapter 28 describes shell/common controls; it does not specify dialog
font pixel values or a numeric font-mapper penalty algorithm. No such evidence
is fabricated. The provisional census and prior GDI report explicitly leave
real acceptance media and integrated font/UI qualification open.

## Decisions

- **f5-10C4 resource size queries (2026-10-20).** Microsoft documents
  [SizeofResource](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-sizeofresource)
  as returning the compiled resource's byte count, independently of loading
  its data. The current kernel32 requests only a size from LdrAccessResource,
  but ntdll incorrectly requires an address output. Accept each output
  independently while retaining module, entry and payload bounds checks;
  [upstream Wine's resource accessor](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/resource.c)
  also treats both outputs as optional. This is an API implementation detail,
  not adoption of NT process architecture for DOS compatibility.
  The regression must load the compiled dialogs PE through the production
  kernel-backed loader, then pass its actual returned resource bytes to the
  USER/GDI harness. Microsoft describes the
  [LoadResource / LockResource sequence](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadresource)
  and [DIALOGEX layout](https://learn.microsoft.com/en-us/windows/win32/dlgbox/dlgtemplateex);
  retain the compiled template and its existing parser. Font bridge statistics
  must return their typed transport/service error in GetLastError when the
  service is unavailable, without treating readiness retries as glyph failures.
  [CreateFontIndirectA](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createfontindirecta)
  creates a logical font; physical realization is separate, so an unavailable
  bridge does not by itself invalidate logical font creation.
  The GUI fixture is spawned directly by the desktop with inherited native
  stderr (fd 2), without Win32 STARTF_USESTDHANDLES. Diagnostic lines use the
  existing CIUKI_SYS_WRITE ABI to that fd, without allocating a console or
  adding a system call. The host harness adapts only this write operation.

- **Non-client bands.** The desktop draws only the frame and caption of a
  top-level window (3-pixel border, 24-pixel caption band). The personality
  owns the menu bar and window scroll bars: a top-level surface is the client
  area plus a menu strip on top (`SM_CYMENU` = 18 px per row, the height
  `AdjustWindowRect` already adds) and scroll-bar bands on the right/bottom
  (`SM_CXVSCROLL`/`SM_CYHSCROLL`). The client origin moves below the strip;
  placement and CONFIGURE carry the surface origin and size, so the f5-09
  placement rules see one rectangle. Child borders (`WS_BORDER`,
  `WS_EX_CLIENTEDGE`/`STATICEDGE`/`DLGMODALFRAME`) and child scroll bars are
  drawn by USER into the parent's surface at `BeginPaint`. Pointer input over
  a band becomes `WM_NC*` with the hit-test code.
- **Painting order.** Without `WS_CLIPCHILDREN`, `InvalidateRect` on a parent
  invalidates the children it covers, and `WM_PAINT` is retrieved for a
  parent before its children (classic USER behaviour).
- **Text.** `GetWindowText(Length)` sends `WM_GETTEXT(LENGTH)` to windows of
  the calling process, so controls own their text; `DefWindowProc` reads the
  stored title.
- **Dialogs.** Templates are parsed with explicit bounds (1 MiB cap for the
  `Indirect` entry points, 120 items). Controls get decreasing z order so the
  first template control is on top and first in tab order. Dialog base units
  come from the dialog font (System font without `DS_SETFONT`); the plan and
  measurements are in `fonts-contract.md` ("Win32 font selection"). Edit,
  list and combo controls with `WS_BORDER` get `WS_EX_CLIENTEDGE` (the 4.0
  three-dimensional look). Modal loops send `WM_ENTERIDLE` (MSGF_DIALOGBOX)
  to the owner when the queue is empty and re-enable the owner on exit.
- **Controls.** BUTTON, STATIC, EDIT, LISTBOX, COMBOBOX (drop-down lists are
  protocol v3 popup windows of class `ComboLBox`) and SCROLLBAR are drawn from
  GDI primitives in the classic colours. comctl32 classes (progress, status
  bars) come from the reused Wine 10.0 `comctl32` (f5-14B), not from USER.
- **Menus.** Menu bars are painted in the strip; popups are topmost
  `#32768` popup windows shown without activation. Tracking is one modal loop
  per thread with keyboard, mnemonic and mouse handling; commands are posted
  (`WM_COMMAND`/`WM_SYSCOMMAND`) unless `TPM_RETURNCMD`. The system menu
  subset is Restore/Move/Size/Minimize/Maximize/Close; Move and Size are
  greyed because protocol v3 has no interactive move/size request.
- **comdlg32.** The directive's `comdlg32` subset is not implemented here:
  f5-14B decided to reuse the Wine 10.0 `comdlg32` build in the builtin
  store, which needs the USER/GDI exports listed by f5-20B. This overlap is
  reported to the lead rather than resolved by duplication.
- **Caret.** The caret is inverted directly on the window surface (no
  `WM_PAINT`), hidden around `BeginPaint`/`EndPaint`, with a blink timer.

Detailed tested coverage and remaining limitations are recorded in
`build/codex/f5-10c-report.md`; API availability is not evidence of complete
Windows 98 behavior or NSIS execution.
