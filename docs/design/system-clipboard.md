# One system clipboard (G5)

Implementation review, 2026-10-23 22:43 CEST. Scope: G5 of
`docs/directives/g1-g6-os-gaps-batch-1.md`, inventory CB-1, CB-3–CB-8 and
KB-10. The remaining obligations below are still G5 gaps, not exclusions
from the directive. This document replaces the incomplete implementer's draft.

## Research and expected behaviour

Primary sources checked against the former implementation:

- Microsoft [Clipboard Operations](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations):
  exclusive access, ownership acquired by EmptyClipboard, multiple formats,
  delayed rendering and memory ownership. This requires a desktop lock shared
  by native and Win32 clients, rather than three unrelated stores.
- Microsoft [Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-formats)
  and [Standard Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats):
  standard IDs, synthesized text and bitmap formats, CF_LOCALE, CF_HDROP,
  private and registered formats. A process-local sequence counter cannot
  describe changes made by another program.
- Microsoft [WM_RENDERFORMAT](https://learn.microsoft.com/en-us/windows/win32/dataxchg/wm-renderformat),
  [WM_RENDERALLFORMATS](https://learn.microsoft.com/en-us/windows/win32/dataxchg/wm-renderallformats),
  and [SetClipboardViewer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setclipboardviewer):
  rendering a requested format does not open the clipboard; rendering on
  owner destruction does. Viewers receive change notifications and maintain
  their existing chain.
- Microsoft [Shell Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard),
  [DROPFILES](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/ns-shlobj_core-dropfiles)
  and [Handling Shell Data Transfer Scenarios](https://learn.microsoft.com/en-us/windows/win32/shell/datascenarios):
  transfer file lists as CF_HDROP; communicate Cut with Preferred DropEffect;
  consume a successfully completed move without deleting a newer clipboard.
- Microsoft code-page mappings:
  [1252](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1252.TXT),
  [437](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT),
  [850](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP850.TXT).
  Tables are deterministic; no third-party implementation is vendored.
- The owner's Windows 98 Resource Kit, Appendix G (`wrkappg_2.htm`, lines
  60–65), assigns Print Screen to the desktop and Alt+Print Screen to the
  active window. Appendix D (`wrkappd_3.htm`, line 180) lists Clipboard
  Viewer. Chapter 25 (`wrkc25_3.htm`–`wrkc25_5.htm`) supplies the Windows 98
  DOS-session context; this implementation does not use NTVDM as its model.
  Media remain read-only and are not redistributed.
- [V7 VGA Technical Reference, table 4-4](https://www.vgamuseum.info/images/doc/headland/video7_v7vga_technical_reference_manual_jun88.pdf):
  BIOS text mode, columns, displayed page offset, rows and character height.
  The VM snapshot validates those guest-controlled fields before reading
  character bytes from B0000/B8000.
- Phoenix [System BIOS for IBM PC/XT/AT Computers and Compatibles](https://vtda.org/books/Computing/Programming/SystemBIOSforIBMPC_XT_ATComputersandCompatibles_Phoenix.pdf),
  keyboard character tables (p. 149) and the 40:19 BIOS work byte: Alt plus
  decimal keypad digits produces an OEM character. The synthetic INT 9
  must accumulate those digits and emit one character when Alt is released;
  focus cancellation must discard an unfinished accumulator.
- Upstream [ReactOS Clipboard Viewer file reader/writer](https://raw.githubusercontent.com/reactos/reactos/master/base/applications/clipbrd/fileutils.c)
  documents interoperable CLP format selection. The 0xC350 encoding here
  is written independently: a four-byte header followed by 89-byte format
  entries (WORD ID, DWORD size, DWORD offset, 79-byte ANSI name).

## Runtime contract

`apps/libdeskui/clipstore.c` implements the one RAM store, instantiated by
`apps/desktop/clipboard.c` in the desktop. The desktop's own folder worker
uses the same store; a recursive mutex serializes worker and UI access.
Clipboard data survive a producer's exit once rendered, but not a desktop
restart. Runtime native text and file paths no longer use `text.clip` or
`files.clip`. Their old VFS providers remain only in host-test builds.

The store holds at most 32 formats and 16 MiB of data. Each GET returns an
independent copy. Incoming surfaces are copied before the producer may
reuse them; outgoing surfaces are fresh, and the existing kernel channel
attachment path grants the receiver read-only access. Rejected PUTs do
not change the store. EmptyClipboard remains a separate destructive
operation, matching Windows; an allocation failure after EmptyClipboard
can therefore leave an empty or partially published clipboard.

One opener excludes all other native, DOS-window and Win32 clients.
QUERY, registration and watching need no open lock; EMPTY and ordinary PUT
require the opener; rendered PUT requires the existing owner. CLOSE adds
an input locale when missing and publishes CHANGED. A disconnected owner
loses unrendered promises; rendered bytes remain. RELEASE handles the
owner window disappearing while other windows in its process remain.
The sequence is global, including synthesized locale placement.

Protocol IDs preserve main's fx-19 messages 42–48 and G3 messages 49–51.
G5 uses native 52/53 (request/event), DOS-window command 54, and v3 80/81
with window and generation zero. Operations are QUERY, EMPTY, PUT, CLOSE,
GET, WATCH, REGISTER, NAME, OPEN and RELEASE. Payloads are explicitly
encoded bytes, not C-structure layouts. Data use bounded XRGB8888
surfaces; `desk_clip_dims` computes their geometry.

A delayed GET sends RENDER to the owner and has a two-second deadline.
Replies remain in a per-client slot when the outgoing channel is full;
a client may have one outstanding clipboard request. Clients wait for
matching serials and retain unrelated UI packets. Native RPC waits retain up to 1024 deferred UI messages and do not
re-enter application callbacks. Menu availability uses change-notification
caching; an actual Paste queries the current sequence before using cached
text, so a delayed notification cannot make Paste use an older copy. The desktop retries replies without
retaining a surface descriptor after a send attempt.

## Clients and formats

- Native text fields and accessories use UTF-16 CF_UNICODETEXT with CRLF
  through their desktop connection. The toolkit exposes UTF-8/LF to apps.
  Non-BMP characters round-trip. ANSI is CP1252; OEM is CP437 or CP850
  according to the Western CF_LOCALE mappings in the core. Missing locale
  uses the desktop's current US or Italian keyboard setting.
- Files and desktop icons publish wide CF_HDROP with Windows drive paths
  and Preferred DropEffect (2 for move, 5 for copy/link). ANSI HDROP is
  accepted as CP1252. A complete move consumes the clipboard only if its
  saved sequence still matches atomically (EMPTY carries the expected
  sequence). The saved sequence comes from CLOSE while publishing, rather
  than a query after releasing the lock. Native Files reads the clipboard on its UI
  thread, outside the background directory scanner. Copy/Paste errors are
  presented through existing dialogs.
- Paint publishes a bottom-up 24-bit DIB and reads a shared DIB, mapping
  received colours into its palette. Paint/Viewer decode 1/4/8-bit indexed,
  16/24/32-bit RGB, contiguous RGB bitfields, and top-down images within
  their existing image bounds. Unsupported compression is refused.
- USER32 uses the v3 store for data, ownership, format enumeration,
  availability and sequence. w32srv retains only the Win32 thread lock,
  viewer chain and atom namespace for this path; its retired data operations
  return ERROR_CALL_NOT_IMPLEMENTED, so they cannot create a second store.
  Registered formats map by
  name between the atom and desktop tables. Global data blocks do not
  consume USER's unchanged 4 MiB metadata allowance. CF_BITMAP crosses
  the channel as CF_DIB and is recreated through the existing GDI bridge.
  WM_RENDERFORMAT, WM_RENDERALLFORMATS and WM_DESTROYCLIPBOARD use the
  owner window. Private-format allocation remains the owner's responsibility.
- DOS windows expose Mark, Copy, Paste and a QuickEdit toggle in their
  window menu. Mark pauses the VM at its existing checkpoints. Text Copy
  reads the visible BIOS text page, trims row-end blanks and publishes
  CF_OEMTEXT plus US locale. Graphics Copy publishes the selected pixels as
  DIB. Enter copies; Esc resumes. Paste converts Unicode into CP437 and
  sends paced set-1 key events, retries queue pressure, folds CRLF into one
  Enter, and uses Alt+numpad for characters without a US key. This is a
  window bridge, not the DOS programmatic clipboard multiplex interface.
- Print Screen and Alt+Print Screen/SysRq capture the composed desktop or
  clipped active window frame as DIB, without a separate display grant.
- `/Programs/clipbrd`, under Accessories > System Tools, shows text, DIBs
  and file lists, lists format names/sizes, subscribes to changes, deletes
  the clipboard, and opens/saves byte formats as Windows 3.1/9x CLP files.
  Export keeps synthesized text as Unicode and bitmaps as DIB. Imports
  validate directory bounds, overlap, duplicates, aggregate size and text
  terminators before replacing data. CLP names must fit 78 ASCII bytes;
  other registered names are refused rather than truncated.

## Remaining G5 obligations and qualification

- DOS INT 2Fh AX=1700h–1705h/1709h is not implemented. A request/completion
  upcall between the V86/DPMI VM and its window needs a reviewed contract;
  a second kernel clipboard or a stale polling mirror would break CB-1.
- CF_PALETTE, CF_METAFILEPICT, CF_ENHMETAFILE, CF_OWNERDISPLAY and display
  metafile/bitmap handle variants lack a cross-process GDI representation.
  They are refused rather than treating process-local handles as bytes.
  Delayed CF_BITMAP is also not advertised; delayed byte formats, including
  DIB, are supported.
- Registered names are bounded to 127 UTF-8 bytes and ASCII case folding;
  other ANSI/OEM code-page families and RLE DIB decoding are not covered.
- DOS selection uses BIOS text geometry, not arbitrary custom CRTC text
  layouts. The full DOS toolbar and persisted properties remain G17 work;
  menu commands and an in-session QuickEdit toggle are delivered here.
- Native delayed producers have the wire operations but no toolkit render
  callback. The implemented delayed-render client is USER32.
- OLE IDataObject clipboard operations are not added by this batch.
- Screenshot capture uses the retained desktop composition. Capturing an
  exclusive DOS/Win32 framebuffer lease is not implemented by this path.
- Host evidence is not guest/hardware evidence. The lead must review the
  whole change, reconcile the existing Setup ABI oracle drift, run the
  leak-enabled suites outside this sandbox, and perform permitted
  integration validation. No QEMU, systemd scope or image build was run.
