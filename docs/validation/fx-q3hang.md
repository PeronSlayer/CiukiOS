# Quake III lease failure and installer supervision

## Research and implementation decisions

- [id Software win_glimp.c](https://github.com/id-Software/Quake-III-Arena/blob/master/code/win32/win_glimp.c):
  `GLW_SetMode` tries the requested full-screen mode, then an enumerated larger
  mode, restores display settings and creates a windowed drawable on failure.
  `GLW_StartDriverAndSetMode` propagates its error to the renderer initialization
  fallback. This released source postdates demo 1.11; the original executable's
  H1 trace is needed to establish the demo's actual behavior.
- [Microsoft ChangeDisplaySettingsA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsa):
  a driver failure is `DISP_CHANGE_FAILED` (-1); `CDS_TEST` validates without
  changing the mode and a null DEVMODE restores the default.
  The current USER facade records success before acquiring its deferred lease.
  Acquire through an offscreen tool window before publishing the process mode;
  the real drawable reuses that lease. Keep exclusive ownership across focus
  changes within the same process, and revoke on a different process's focus.
  A late lease failure must also stop automatic requests on every GL frame.
  Reuse an already accepted identical mode only without `CDS_RESET`; an
  explicit reset must acquire a fresh lease and expose a new driver failure.
- [Microsoft WaitForInputIdle](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-waitforinputidle):
  initial input-idle is a process latch, satisfied when any GUI thread waits
  with no pending input. A finite timeout returns WAIT_TIMEOUT; failures return
  WAIT_FAILED. Implement the directive's explicit WAIT_FAILED rule for console
  and non-GUI children. A blocking wait must release USER's lock and service
  incoming synchronous messages without dispatching arbitrary posted messages.
- [Microsoft PeekMessageA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-peekmessagea)
  gives `PM_NOYIELD` its input-idle suppression semantics. An empty yielding
  PeekMessage must publish initial idle too; otherwise a game that continuously
  polls its queue could strand its launcher's WaitForInputIdle.
  [Wine's message implementation](https://github.com/wine-mirror/wine/blob/master/dlls/win32u/message.c)
  also services synchronous sends during WaitForInputIdle and distinguishes
  yielding message retrieval from `PM_NOYIELD`. No Wine code is copied.
- [Microsoft IsHungAppWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-ishungappwindow)
  documents the five-second hung-app check. The directive applies this interval
  to renewed prompts after Wait/Cancel. Ghost-window responsiveness is a desktop
  responsibility; this later Windows API is not evidence of Windows 98 ghosting.

The private T23 capture was read in place. It contains three failed grants with
EIO, while the kernel grant transaction already resumes desktop presentation on
failure. The capture alone does not identify a stopped instruction or prove a
particular lock deadlock. Host reproduction and remaining hardware evidence are
reported in `build/codex/fx-q3hang-report.md`.

The runtime fault selection is `f5:q3-lease-eio run=<8 hex digits>`, QEMU only.
The supervisor ordinarily skips bootstrap for test requests; this exact
selection is the sole exception so the ordinary desktop can run against the
injected grant failures. A shared predicate gates bootstrap and injection;
other tests and physical boot retain their existing behavior.
