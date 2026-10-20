# f5-20B — USER and GDI exports for the reused Wine upper DLLs

Directive: `docs/directives/f5-20-core-exports-for-reused-dlls.md` part B and
amendment 1 items 1–3. Host evidence only (no QEMU, no image build). Report
with the per-symbol table: `build/codex/f5-20b-report.md`.

## Sources and decisions

- **Unicode layer (USER and the amendment's KERNEL32 functions).** Microsoft
  "[About the Microsoft Layer for Unicode](https://learn.microsoft.com/en-us/previous-versions/ms812670(v=msdn.10))":
  the layer converts every string to ANSI with the default system code page,
  calls the ANSI API and converts returned strings back; it adds no features
  and no surrogates. Decision: the ANSI USER window manager and KERNEL32 stay
  authoritative; the W entry points convert through code page 1252. Window
  procedures, classes and dialog procedures record whether they take Unicode;
  messages crossing the boundary are translated (text, characters,
  CREATESTRUCT, list/combo strings, EM_GETLINE, WM_GETDLGCODE).
  "[CallWindowProcW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-callwindowprocw)":
  GetWindowLong(GWL_WNDPROC/DWL_DLGPROC) may return "a special internal
  value meaningful only to CallWindowProc", which "handles Unicode-to-ANSI
  conversion". Decision: a procedure of the other kind is handed out as a
  0xFFFFxxxx handle, decoded by CallWindowProc and SetWindowLong;
  IsWindowUnicode reports the kind of the window's procedure.
- **wsprintf.** "[wsprintfA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-wsprintfa)":
  cdecl, 1,024-byte buffer, flags `- # 0`, width, precision, types c C d hc hC
  hd hs hS hu i Ix lc lC ld li ls lS lu lx p s S u x X; a zero `hc`/`lc`
  character is ignored. Implemented with 64-bit values printed in 32-bit
  steps (USER links no compiler runtime).
- **DDEML.** "[DdeConnect](https://learn.microsoft.com/en-us/windows/win32/api/ddeml/nf-ddeml-ddeconnect)"
  returns 0 with DMLERR_NO_CONV_ESTABLISHED when no server answers. CiukiOS
  global memory is private to a process, so no inter-process DDE transport
  exists: instances, string handles (shared, case-insensitive), data handles
  and name registration are implemented; DdeConnect reports no partner.
- **GetFileSecurityA/W** (amendment item 2): both fail with
  ERROR_CALL_NOT_IMPLEMENTED (no file security on FAT under Windows 98);
  [GetFileSecurityA](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfilesecuritya).
- **NtDeleteKey** (amendment item 3):
  "[ZwDeleteKey](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwdeletekey)":
  the handle must have DELETE access; afterwards every handle to the key is
  invalid. Decision: the w32srv refuses an empty name only for a predefined
  root (a root constant, or a handle on the key a root names); otherwise it
  deletes the handle's key, whose handles then report ERROR_KEY_DELETED.
  Unlike NT (STATUS_CANNOT_DELETE with subkeys), the Windows 98 service
  deletes the subtree, as its RegDeleteKey does.
- **Collation.** CompareStringA/W and LCMapStringA/W share one three-level
  collation (base letters, diacritics, case; word sort ignores hyphen and
  apostrophe until all levels are equal; SORT_STRINGSORT treats them as
  symbols), per Microsoft "[CompareStringW](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-comparestringw)"
  and "[LCMapStringW](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-lcmapstringw)";
  LCMAP_SORTKEY keys compare like CompareString (host-tested pairwise).
- **Metafiles.** [MS-WMF](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/)
  record layouts (META_ESCAPE 2.3.6.1 and the embedded-EMF MFCOMMENT
  chunks with the 'WMFC' identifier) and [MS-EMF](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-emf/)
  record layouts; GetWinMetaFileBits keeps the EMF embedded so
  SetWinMetaFileBits restores it exactly.
- **Uniscribe.** Microsoft "Uniscribe Functions" reference; rule
  core-uniscribe of `config/wine-upper.json`: a left-to-right single-script
  subset with characters as glyphs (SCRIPT_ANALYSIS.fNoGlyphIndex).

## Host evidence

| Test | Covers |
| --- | --- |
| `tests.host.win32.dialogs.test_dialogs.DialogTests.test_f5_20b_upper_user` | Unicode classes/windows/messages/subclassing/controls/SPI, Unicode dialogs, menus/MENUINFO/item rectangles/accelerators, Char*/IsChar*/OEM/wsprintf, DDEML, desktop/ancestors/hit testing/window info/deferred positions/regions/monitors/DPI/system timers/TrackMouseEvent/DragDetect/rectangles, clipboard formats, icons, PrivateExtractIcons, DlgDirList |
| `tests.host.win32.dialogs.test_dialogs.DialogTests.test_f5_20b_upper_gdi` | regions, ExtCreatePen, PolyPolyline, GdiAlphaBlend, GetBitmapBits, layout, halftone palette, colour spaces, StartDoc/ExtEscape, DEVMODE conversion, character sets, GdiGetCharDimensions, GetCharWidthInfo, GetTextCharsetInfo, GetCharacterPlacementW, Unicode font enumeration, Uniscribe, metafiles (recording, playback, copies, files, EMF scaling, WMF/EMF conversion) |
| `tests.host.win32.test_kernel32.Kernel32Files.test_f5_20b_unicode_layer_and_collation` | the fifteen amendment functions over the ANSI paths, CompareString/LCMapString A/W, sort-key order |
| `tests.host.win32.test_registry.RegistryTests.test_f5_20b_nt_delete_key_by_handle` | NtDeleteKey by handle on the production w32srv |
| `tests.host.win32.test_kernel_loader` (production store) | every user32/gdi32 decision and the amendment's kernel32 names resolve through the production loader |
