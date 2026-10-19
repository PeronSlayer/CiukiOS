# F5-11 DirectDraw implementation record

Research before implementation, 2026-10-19. The checkout contains a census
placeholder for ddraw.dll, the f5-10B borrowed DIB/DC entry points, and the
f5-08 kernel-copy presenter. USER32 consumes activation events, but has no
public lease request/event or visible-window clip-list interface. The lead
has been asked for that boundary as required by the directive. The f5-08,
f5-09 and f5-10B scratch reports are absent in this worktree; their checked-in
design/validation records and actual headers are the integration reference.

Primary references inspected:

- Microsoft's [DirectDraw SDK header](https://raw.githubusercontent.com/apitrace/dxsdk/master/Include/ddraw.h),
  archival SDK mirror: generation-specific vtable order, old descriptor and
  capability layouts, IID values and HRESULT values. This mirror is a later
  SDK retaining the DX1/3/5/6/7 layouts, not a verified DirectX 7 distribution.
- Microsoft [DirectDrawCreateEx](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-directdrawcreateex):
  only IID_IDirectDraw7 is valid; aggregation is refused.
- Microsoft [Lock](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-lock):
  subrectangle pointers, busy versus drawing states, DX7 default WAIT.
- Microsoft [GetDC](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-getdc):
  the surface remains implicitly locked until ReleaseDC.
- Microsoft [Restore](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-restore):
  implicit flip-chain children restore with the parent; restored video data
  must be reloaded by the client.
- Microsoft [GetClipList](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawclipper-getcliplist)
  and [BltBatch](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-bltbatch):
  sized region output and unsupported batch transfers.

The contract's archived [DirectX 7 interface reference](https://library.thedatadungeon.com/directx7/devdoc/live/directx/ddref_0khj.content.htm)
and [return codes](https://library.thedatadungeon.com/directx7/devdoc/live/directx/ddref_6u9j.content.htm)
were requested but unavailable through the research transport. Period SDK
qualification therefore remains explicit; no Wine DirectDraw implementation
is used or copied.

Implementation decisions: use the existing scalar DIB primitives and borrowed
surface DCs. Keep allocation class separate from backing storage: explicit
offscreen SYSTEMMEMORY survives deactivation, primary and virtual video
surfaces become lost. Never acquire a display grant or map the framebuffer.
Use half-open rectangles and snapshot overlapping source data before writes;
clipping preserves stretch phase. Report software-provided blit/flip/palette
features in the hardware capability record as the contract requires. Pacing
uses absolute 60 Hz monotonic slots, never claims hardware retrace.

## Integration boundary awaiting the lead

The DirectDraw implementation is in `sdk/win32/ddraw/`, built by
`scripts/win32/ddraw.py` and installed as the authoritative builtin. No kernel
ABI number was added. The copied-record call is the actual frozen
`CIUKI_SYS_W32 = 96`, with W32_DISPLAY operation 0x201; the directive's older
call-101 wording is not used to renumber it.

The native adapter deliberately resolves these proposed USER exports instead
of accessing USER's private window ledger or opening a second desktop channel:

```c
DDRESULT W32API w32_user_display_lease(uintptr_t hwnd, DWORD width,
    DWORD height, DWORD bpp, struct dd_lease *out);
void W32API w32_user_display_release(uintptr_t hwnd);
void W32API w32_user_display_poll(uintptr_t hwnd, struct dd_lease *out);
DDRESULT W32API w32_user_display_clip(uintptr_t hwnd, RECT *out, DWORD *count);
```

`dd_lease` is declared in the DirectDraw header. USER must preserve its own
wire-window id/generation, deliver activation on the owning GUI thread, retain
and re-request the logical mode after activation, expose the current token,
framebuffer generation and pitch, and cancel a pending request on release.
`poll` must be a nonblocking read of cached state; it can be called from the
pacing worker and must not synchronously call the GUI thread. `clip` returns
half-open visible rectangles in screen coordinates, with a NULL-output size
query and `DDERR_MOREDATA` for insufficient capacity. It must also be safe to
query from the presentation worker. Until this boundary is supplied, native
full-screen entry returns `DDERR_UNSUPPORTED` and window clip-list access
returns `DDERR_NOCLIPLIST`. Host transport substitution does not close this gap.

The windowed primary is screen-sized. Blits intersect the screen-coordinate
clip list, preserving the source sampling phase. Publication uses public USER
GetDC/ReleaseDC and ClientToScreen plus GDI StretchDIBits: USER owns the root
v3 surface, visible region, damage and WM_PAINT bookkeeping. The windowed shim
retains exclusive API behaviour but resizes the desktop window through public
USER calls, binds an automatic window clipper and copies flips into the
primary. Its publication starts at client origin (0,0). No compositor or kernel
code was changed.

Primary Unlock, ReleaseDC, palette changes and blits dirty the retained primary.
`w32_ddraw_tick` drains the latest state at an absolute 16,666,667 ns slot; the
native adapter supplies a 1 ms worker. Flip WAIT and WaitForVerticalBlank wait
for the software deadline; GetScanLine is the phase within the same clock,
GetMonitorFrequency is 60 and GetVerticalBlankStatus is FALSE. NOVSYNC does
not assert a hardware retrace or bypass the presenter's kernel admission rule.
The native worker and lease/window adapter still require integrated validation.

On revocation or a new token/generation, primary, implicit back buffers and
virtual-video surfaces become lost. Explicit offscreen SYSTEMMEMORY remains
valid and retains its bytes. A new matching lease alone does not clear lost
state: Restore must be called. Implicit children restore with their parent;
explicit attachments restore separately. Video allocations remain reserved
against the reported budget but their content is cleared on Restore. Wrong
logical/native lease dimensions are refused with DDERR_WRONGMODE. Palette and
gamma state are reapplied after a regrant. Host replay uses the production v3
packet decoder and an explicitly project-authored wire recording, not a guest
or Windows trace.

Host evidence is under `tests/host/win32/ddraw/`. The 81 transfer SHA-256 pins and six authored fixture-scene pins are derived
from an independent Python integer-pixel/Boolean oracle for 8/16/32 bpp. The
host allocator checks canaries immediately outside allocations; client buffers
and padded rows have separate guards. ASan/UBSan run with LeakSanitizer disabled
because this sandbox cannot support its ptrace requirement; explicit allocation
and surface-byte counters must return to zero. PE compilation separately asserts
108/124-byte descriptors, 32-byte pixel format, 100-byte blit effects and the
1064/1072-byte device identifiers. DDCAPS dispatch accepts 172, 316, 364 and
380 bytes (DX6 and DX7 have the same size); invalid sizes fail before copying.
The PE disassembly audit rejects x87/MMX/SSE instructions.

The authored `ddraw-exercise.exe` is packaged among the builtin fixtures and
named in both unrun suite cases. It is an exercise driver, not a kernel evidence
producer. The lead still connects selector/controller observations and supplies
the native USER seam. Original-game/Diablo menu execution and the 2 s desktop
recovery gate remain guest evidence; no QEMU or image build was run here.

Publication retains the surface and parent and releases the DirectDraw mutex
across USER clip/window callbacks. The pacing worker retains each device while
walking the list. A reentrant-release regression checks buffer lifetime when
a callback releases the application's final surface/device references. Attached
flip buffers must retain matching pitch, extent and RGB masks; client rebinds
are checked again before rotation.

The f5-08/09/09B/10B reports were subsequently located and reviewed read-only
in the main checkout's `build/codex/`. Their DIB/palette and lease tuple contracts
match the checked-in headers; they do not supply the missing public USER seam.
Final application build and all 13 focused tests pass. Full Win32 discovery runs
140 tests with one separately reproduced failure in the unchanged USER32
teardown harness and 11 skips; see the f5-11 report for the unabridged result.
