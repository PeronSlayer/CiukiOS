# One system clipboard (G5, G5b)

Implementation review, 2026-10-23 22:43 CEST. Scope: G5 of
`docs/directives/g1-g6-os-gaps-batch-1.md`, inventory CB-1, CB-3–CB-8 and
KB-10. The G5b contract below completes Amendment 1; qualification limits and
remaining G17 work are recorded at the end.

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

### G5b completion contract (2026-10-24)

Research: Microsoft's [Q67675, Access to the Windows Clipboard by
MS-DOS-Based App](https://jeffpar.github.io/kbarchive/kb/067/Q67675/)
documents the enhanced-mode WINOLDAP interface, also indexed in
[RBIL, Windows multiplex functions](https://fd.lod.bz/rbil/interrup/windows/index.html)
and the [mirrored original WINOLDAP register tables](https://wiki.nox-rhea.org/back2root/ibm-pc-ms-dos/interrupts/old/ms_interrupt_list-22-30).
This is the Windows 9x V86 session interface, independent of NTVDM.
AX=1700h identifies support (AL major, AH minor); 1701h opens, 1702h
empties, 1703h copies SI:CX bytes from ES:BX in format DX, 1704h returns
the size in DX:AX, 1705h copies into ES:BX, 1708h closes, and 1709h
returns available bounded transfer space in DX:AX. Boolean calls return
AX=0 on failure and nonzero on success. Only CF_TEXT/CF_OEMTEXT are
required here; all other formats fail without altering the store.

The VM has one transient request slot, not a clipboard. A VM interrupt
publishes a serialised request, wakes WAIT_PRESENT with EVENT_CLIPBOARD,
and waits on a completion gate for at most two seconds. The owner-only
CLIP_REQUEST/CLIP_COMPLETE operations copy a bounded 64 KiB transfer;
serial and VM generation reject late completions. No guest pointer reaches
the desktop. Failure, timeout and owner death release transfer memory.
DPMI 0300h and default INT 2Fh reflection use the same real-mode buffer
semantics and the existing host return stub. The DOS client executes each
operation against the existing desktop RPC store, retaining its exclusive
open across guest calls; no polling copy or file mirror is introduced.

Microsoft [Standard Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats),
[Clipboard Operations](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations)
and [Using the Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/using-the-clipboard)
require format-specific handle destruction and owner-display messages.
GDI handles are serialized as DIB, LOGPALETTE, metafile bits with the
METAFILEPICT mapping/extents, or enhanced-metafile bits. The receiving
process creates its own handles; rendered bytes survive owner exit.
EmptyClipboard destroys system-owned local handles and the stored bytes;
unrendered promises and owner-display state disappear on owner exit.
Delayed CF_BITMAP retains its advertised format while rendering a DIB.
Native producers receive RENDER/RENDERALL/DESTROY callbacks; normal window
close renders promises before sending CLOSE while its channel remains live,
with rendered
PUT allowed while the requester owns the open lock. Registered names use
the repository's Unicode case-fold table, shared with the registry.

Full-screen DOS Print Screen publishes the visible BIOS text page as
CF_OEMTEXT; graphics modes leave the clipboard unchanged (Amendment 1's
Windows 98 rule). The input path only queues the request: the VM worker
captures text after synchronising the VGA aperture, and its DOS client
publishes through the ordinary store. QuickEdit and Exclusive mode are
saved per program (interactive shell has its own key) through a shared
settings API for G17. Exclusive mode gives mouse input to the DOS program and suppresses automatic
QuickEdit selection; it does not disable the programmatic clipboard.
The Resource Kit Chapter 25, table 25.2 (`wrkc25_3.htm`), distinguishes
Exclusive mode, fast paste and the Alt+Print Screen application shortcut.

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
GET, WATCH, REGISTER, NAME, OPEN and RELEASE. G5b appends DISPLAY and
DISPLAY_REPLY plus DISPLAY_EVENT for bounded owner-display viewer messages. Payloads are explicitly
encoded bytes, not C-structure layouts. Data use bounded XRGB8888
surfaces; `desk_clip_dims` computes their geometry.

A delayed GET sends RENDER to the owner and has a two-second deadline.
Text conversion can request the promised source first, then a promised
CF_LOCALE, within that same deadline; conversion never reads an unrendered
locale block. Native self-reads render their promised text/bitmap source
and locale before entering the synchronous GET.
Replies remain in a per-client slot when the outgoing channel is full;
a client may have one outstanding clipboard request, with owner rendering
PUT/QUERY/NAME/REGISTER allowed while that owner waits on its own delayed GET. Clients wait for
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
  name between the Unicode-aware atom and desktop tables. Global data blocks
  do not consume USER's unchanged 4 MiB metadata allowance. CF_BITMAP
  retains its format ID, carries DIB bytes, and is recreated through GDI.
  Palette, metafile-picture and enhanced-metafile objects (including display
  variants) are serialized and reconstructed per process. Returned handles
  remain valid after CloseClipboard until replacement or process exit.
  WM_RENDERFORMAT, WM_RENDERALLFORMATS and WM_DESTROYCLIPBOARD use the
  owner window. Private-format allocation remains the owner's responsibility.
- DOS windows expose Mark, Copy, Paste and a QuickEdit toggle in their
  window menu. Mark pauses the VM at its existing checkpoints. Text Copy
  reads the visible BIOS text page, trims row-end blanks and publishes
  CF_OEMTEXT plus US locale. Graphics Copy publishes the selected pixels as
  DIB. Enter copies; Esc resumes. Paste converts Unicode into CP437 and
  sends paced set-1 key events, retries queue pressure, folds CRLF into one
  Enter, and uses Alt+numpad for characters without a US key. This is a
  window bridge. The G5b programmatic multiplex path is the request/completion
  interface above, using the same store and text conversions.
- Print Screen and Alt+Print Screen/SysRq capture the composed desktop or
  clipped active window frame as DIB, without a separate display grant.
- `/Programs/clipbrd`, under Accessories > System Tools, shows text, DIBs
  and file lists, lists format names/sizes, subscribes to changes, deletes
  the clipboard, and opens/saves byte formats as Windows 3.1/9x CLP files.
  Export keeps synthesized text as Unicode and bitmaps as DIB. Imports
  validate directory bounds, overlap, duplicates, aggregate size and text
  terminators before replacing data. CLP names must fit 78 ASCII bytes;
  other registered names are refused rather than truncated.

## Qualification and remaining work

- G5b adds host coverage for register results, V86 guest buffers, DPMI return
  frames, owner isolation, request serials/deadlines, text-mode full-screen
  capture, graphics no-op, native producer callbacks, Unicode registration,
  GDI object reconstruction/lifetimes and owner-display viewer messages.
- Kernel transfers use two temporary 64 KiB PMM allocations at peak (VM
  request and native marshalling); kheap's 2040-byte limit forbids using
  kmalloc for them. Allocation failure returns API failure, not a VM fault.
  The existing DOS memory/admission limits are unchanged.
- Owner-display messages marshal viewer parameters, rectangles and names.
  WM_PAINTCLIPBOARD gives the owner a local HGLOBAL PAINTSTRUCT and local
  GDI DC backed by a bounded bitmap, then returns a DIB for the viewer's DC.
  Neither an HGLOBAL nor an HDC from one process is used by another.
  The desktop authenticates the answering owner and assigns a distinct request
  nonce, preventing late replies from matching a reused client slot. Win32
  requests also bind to the owner HWND observed by the caller.
- Registered names remain bounded to 127 UTF-8 bytes. Case comparison uses
  the existing Unicode full-fold table, without normalization. ANSI/OEM
  families beyond 1252/437/850 and RLE DIB decoding remain outside G5b.
- DOS selection and full-screen text capture use BIOS geometry, rather than
  arbitrary custom CRTC text layouts. The complete DOS toolbar and property
  pages remain G17 work; `apps/dosbox/clipboard_settings.h` provides the
  shared per-program load/save API for QuickEdit and Exclusive mouse mode.
  The G5b runtime preference suppresses automatic QuickEdit in Exclusive
  mode; G17 owns the complete exclusive pointer-capture UX.
- CLP export still retains supported byte formats and DIB; exporting GDI
  metafile/palette handles or owner-display callbacks is not added here.
- OLE IDataObject operations and exclusive Win32 framebuffer screenshots
  remain outside Amendment 1. Full-screen DOS text publication and graphics
  no-op are covered here.
- Host evidence and sandbox limitations are in `build/codex/g5b-report.md`.
  Guest/hardware integration and the lead's cross-review remain required.
