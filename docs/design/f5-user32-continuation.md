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

## fx-w32idle: blocking message waits

Round 3 applies the same event-driven contract to overlapping coordinators.
Comparing the production session wake path with Microsoft's blocking
[GetMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessage)
and [MsgWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjects)
contracts and Wine's
[queue wait/recheck loop](https://github.com/wine-mirror/wine/blob/master/dlls/win32u/message.c)
shows two private bookkeeping requirements: a session that prevents delivery
invalidates the undelivered power turn's notification, and its power deadline
cannot schedule a wake while that session retains priority. Once reset,
cancelled or completed, the session releases priority; the usual wake step
evaluates the deferred power turn against the refreshed clock and signals it
once if still pending. Delivered power callbacks retain their existing state.
Hive commit deadlines remain eligible during a hold; finite sessions keep
their own deadlines. These are CiukiOS coordination decisions derived from
the blocking model, rather than claims that Windows exposes the same private
protocol. The original queue
order, timer synthesis and round-2 race fixes remain unchanged.

Round 2 refines the same blocking contract using the Microsoft references and
Wine's [wait_message implementation](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/win32u/message.c):
internal event processing retains an absolute application timeout and checks
queue readiness again. A timer that reaches its deadline after readiness was
published must cause an immediate check, never an infinite sleep. Session and
power turns notify once per pending state; nested USER loops consume duplicate
latches and defer session RPCs until the active callback returns. After an
internal wake, USER checks application objects/queue without internal services
before retrying, so repeated service activity cannot hide input or a timeout.
The service refreshes its monotonic clock on entering each loop and immediately
before dispatching each request. Its session, power and shutdown windows thus
start at request time even after an indefinite idle sleep. These are host-tested
race and loop-order requirements, not guest residency measurements.

Research and implementation decision (host evidence only): Microsoft
[GetMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessage),
[WaitMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-waitmessage),
[MsgWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjects),
and [About Messages and Message Queues](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues)
specify blocking queue waits, optional kernel objects, and low-priority paint
and timer synthesis. WaitMessage requires new arrivals; MWMO_INPUTAVAILABLE
also accepts existing unread input. Preserve these distinctions and the current
sent/posted/input/paint/timer delivery order. These live documents describe the
retained API contract; they are not hardware evidence for Windows 98.
The locally supplied Windows 98 Resource Kit, chapter 28, corroborates the
historical model: `wrkc28_8.htm` (User) describes asynchronous input and a
separate queue for each Win32 thread; `wrkc28_4.htm` (Multitasking) describes
preemptively scheduled Win32 threads. Its cooperative Win16 scheduling does
not justify periodic RPCs in a Win32 message wait.

Wine's [dlls/win32u/message.c](https://github.com/wine-mirror/wine/blob/master/dlls/win32u/message.c)
(get_server_queue_handle, wait_objects, NtUserMsgWaitForMultipleObjectsEx and
NtUserGetMessage) waits on the server queue handle alongside application
handles. Its wait_message rechecks driver work on queue wakes while retaining
the application's absolute timeout. Use that event-driven model; no Wine code
is copied.

Repository baseline: the production USER harness records four end-session
RPCs in 1000 ms per idle GUI thread. The guest service extends active_until
by 20 ms on every fragment, so four isolated replies keep it runnable for
80 ms/s (8 percent, excluding actual RPC work). USER additionally wakes every
10 ms and the kernel wait every millisecond. These are separate costs.

Decision: retain the existing queue and synthesis code. Extend W32_WAIT with
an internal service wake that observes desktop channel readiness and a per-thread
session latch. USER drains the transport and rechecks queue/object conditions
on that wake; only a real application wait result leaves MsgWait. Timers supply
the nearest real deadline, including kernel waitable timers. The registry
service signals the authenticated owning thread when session/power coordination
advances, and USER fetches session work only after consuming that latch.
The service waits on accepted channel readability/closure or a new connection;
fragmented transactions no longer require a runnable grace period. Dirty hive
commits retain their two-second deadline. Display defaults are refreshed while
processing traffic, rather than by a permanent idle polling deadline.
After desktop disconnection, USER keeps waiting on the session latch alone:
an already observed channel hangup cannot repeatedly wake it. Readiness uses
retained service endpoint identities, and pending secure links must name the
exact server subject and an endpoint it still owns, matching ACCEPT.
