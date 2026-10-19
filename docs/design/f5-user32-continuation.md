# F5-10A continuation boundary

Research before implementation, 2026-10-19. Amendment 1 is the scope.

Primary sources inspected:

- Microsoft Windows 98 Resource Kit, local main checkout
  `build/rskt/wrkc24_3.htm`, Display Minidrivers: the 32-bit DIB engine
  performs memory graphics. The [archived chapter](https://library.thedatadungeon.com/msdn-2000-04/rk98book/html/wrkc24_3.content.htm)
  was unavailable through the web transport; the local book was read.
- Microsoft [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
  and [DestroyWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-destroywindow):
  sibling insertion, topmost bands, owner constraints, automatic dependent
  destruction, and the public same-thread destruction restriction.
- Microsoft [ExitThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitthread)
  and [TerminateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminatethread):
  ordinary detach versus forced termination without DLL thread-detach callbacks.
- Microsoft [DrawIconEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawiconex),
  [CreateDIBitmap](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibitmap),
  [GetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclipboarddata),
  [palette messages](https://learn.microsoft.com/en-us/windows/win32/gdi/palette-messages),
  and [SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa):
  separate image/mask copying, combined AND/XOR, DIB conversion, palette
  notification recipients and action-dependent setter arguments.
- Microsoft [Interpreting Mouse Axis Data](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418272(v=vs.85)):
  absolute DirectInput mouse coordinates accumulate raw relative motion from
  an unspecified origin, independently of the cursor and Control Panel speed.

The live Microsoft references document retained API semantics, not measured
Windows 98 parity. Period Platform SDK retrieval remains a source limitation.
The checked-in f5-10B and f5-11 records and actual published interfaces are the
integration reference; no Wine USER implementation is used.

Decisions: extend v3 only with WINDOW_ORDER, preserving existing opcode values
and frames. USER keeps ownership of its window ledger, event delivery and
surfaces; GDI owns conversion and raster operations through the existing
1068-byte PE32 DIB descriptor. Cross-thread dependent destruction uses a private
same-process sent request on the owning thread. Forced cleanup releases USER
resources without invoking the victim's procedures. DLL detach integrates normal
thread/process exit. The existing ntdll `w32_server_exchange` holds the shared
PEB lock over the entire BEGIN/WRITE/READ transaction for USER, kernel32 and
advapi32; DLL-local request-buffer locks do not replace this process-wide lock.

DirectDraw uses the public lease request/release and cached poll/visible clip
seam in `w32/user_surface.h`. Only the owner thread sends lease requests and
consumes activation events. Workers read copied cached state without desktop
I/O or synchronous GUI sends. The desktop already retains the logical mode and
performs same-mode regrant on reactivation; USER preserves that tuple and clears
it immediately on deactivation/release. SPI values are shared by GUI threads
within the process and are never persisted to native desktop settings.

## Implementation record (continuation `wt/f5-10a2`)

Additional primary sources: Microsoft
[IDirectInputDevice8::SetEventNotification](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417927(v=vs.85))
(loss of acquisition is a device state change that sets the event),
[WM_PALETTECHANGED](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-palettechanged)
(sent to every top-level window, including the source; wParam = the window that
changed the system palette) and
[SetDIBits](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setdibits)
/ [GetDIBits](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdibits)
(scan lines are numbered in the caller's DIB orientation).

- **WINDOW_ORDER (77).** Wire and semantics in `docs/design/desktop-shell.md`.
  `SetWindowPos` sends it for top-level windows after PLACE and restacks the
  local sibling ledger with the same rules; `SetWindowLong(GWL_HWNDPARENT)`
  sends OWNER for a same-process top-level owner and refuses cycles and child
  owners with error 87. The SWP sentinels never name a window, even when a
  window handle has the same numeric value.
- **Lifecycle.** `u32_destroy` has three modes: NORMAL (DestroyWindow; a
  dependent owned by another thread of the process is destroyed on that thread
  by the private `U32_DESTROY` sent request, honoured only when USER itself set
  the window's request flag), VICTIM (forced: no procedure of the victim runs,
  live dependents still get WM_DESTROY on their own thread) and TEARDOWN
  (process detach: no callbacks, no sends, the desktop connection close drops
  every window at once). `user32.dll` now has the entry `UserDllMain`:
  DLL_THREAD_DETACH destroys the exiting thread's windows on that thread and
  releases its timers, hooks, clipboard lock, desktop requests and queue
  state; DLL_PROCESS_DETACH tears everything down, including classes and
  icons. A thread without USER state costs no service transaction.
  `TerminateThread` runs no DLL detach, so kernel32 calls the exported
  `w32_user_thread_cleanup(tid)` when user32 is already loaded: the kernel's
  `SYSTEM/TERMINATE_THREAD` now reports a same-process victim TID in `result`
  (execution ABI), and a self-termination through the pseudo handle releases
  first. The service identity request `W32R_USER_CLEANUP` (index = TID, zero =
  the whole PID) is a safety net for records a failed free left behind.
- **Serialization.** USER's directory client, kernel32 and the advapi32 facade
  reach w32srv through ntdll `w32_server_exchange` (FastPebLock held across
  BEGIN/WRITE/READ): one transaction at a time per process. Host evidence
  checks the actual PE imports.
- **GDI integration.** `GetClipboardData(CF_BITMAP)` converts any CF_DIB the
  service holds (1/4/8/16/24/32 bpp, BI_RGB or BI_BITFIELDS, colour tables,
  either orientation, V4/V5 headers) with gdi32 `SetDIBits` into a 32-bpp
  top-down section (the desktop is XRGB8888); truncated data is refused.
  gdi32's `scan_transfer` numbered scan lines by the bitmap's storage, which
  flipped mixed-orientation transfers; it now follows the caller's DIB.
  `DrawIconEx`: DI_NORMAL = AND then XOR through `w32_gdi_draw_icon`, DI_IMAGE
  or DI_MASK alone = SRCCOPY of that bitmap, a flicker-free brush fills first,
  invalid flags or negative sizes fail with 87. Palette callbacks: gdi32
  reports under its own lock, so USER only appends to a lock-free ring drained
  at the next USER entry (lock-order inversion otherwise); 0x030f becomes
  WM_PALETTEISCHANGING and 0x0311 WM_PALETTECHANGED, posted to every top-level
  window with wParam = the realising window (AnimatePalette: the foreground
  window). Posting rather than sending is the recorded deviation that keeps
  WndProcs out of gdi32's critical section. Activation sends
  WM_QUERYNEWPALETTE before the focus arrives.
- **SystemParametersInfo.** Process-global, never persisted
  (SPIF_UPDATEINIFILE accepted without writing); SPIF_SENDCHANGE sends (SendNotifyMessage)
  WM_SETTINGCHANGE to this process's top-level windows. Setters follow the
  per-action argument conventions: uiParam values (beep, icon spacing with a
  NULL pvParam, title wrap, menu alignment, double-click size and time, button
  swap, show sounds, keyboard preference, screen saver running with the old
  value in pvParam), pvParam values (mouse speed 1..20) and structures (mouse
  thresholds, icon title font, non-client, minimised and icon metrics, work
  area, animation). GetSystemMetrics reflects swap, double-click size and icon
  spacing.
- **DirectDraw seam** (`w32/user_surface.h`, value tuple in
  `w32/display_lease.h`): `w32_user_display_lease` (owner thread only; a
  same-mode request returns the cached grant without a transaction), `release`,
  and the worker-safe `poll`/`clip`, which copy a per-window seqlock snapshot
  of the lease tuple and the visible client area (screen coordinates, clipped
  by every ancestor's client area and the screen, minus this process's visible
  top-level windows above the root; other processes' windows are the
  compositor's). A busy slot leaves the caller's tuple unchanged instead of
  reporting a revocation. Desktop events reach the owner thread through USER's
  queue: deactivation clears the tuple before any procedure runs, a GRANTED
  event for the retained mode installs the regrant, a failed grant drops the
  retained mode.
- **DirectInput absolute axes.** Mouse X/Y accumulate the raw relative counts
  in both axis modes; absolute mode reports the running sum (no cursor
  position, no DIPROP_RANGE clamp, no reset on read), relative mode the delta
  since the last read. The wheel follows the same rule.
