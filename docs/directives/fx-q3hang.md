# fx-q3hang: Quake III start must never freeze the machine; the installer's launch must not hang it (T23 trial 5bb73dba)

- **Written:** 2026-10-26 16:31 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Evidence:** owner's T23, private serial capture in the lead's checkout legacy/local/physical/5bb73dba/serial.log (read it, never copy it into the tree).
  - **Game start.** After the three failed full-screen leases (result=-5, cause fixed separately in fx-biosvm), the whole machine stops. Periodic [cpu]/[mem]/[kernel] records stop and the next serial line is the loader of a forced reboot. The owner saw a grey square, probably the 640x480 game window, then everything frozen. The previous boot shows the same sequence.
  - **Installer.** At the end of the original VISE installer the owner left the option to start the game. quake3.exe pid 10 started while Q3ADemo.exe pid 8 was still running. The installer then showed "not responding" repeatedly and was killed through End Task.
  - In QEMU (no BIOS VM failure) the game and the installer work.

## Scope
1. **Reproduce on the host.** Use the production Win32/user32/gdi32/opengl32 and kernel display-lease paths, with lease failures injected (EIO) exactly like the trial. Follow what quake3.exe 1.11 does after ChangeDisplaySettings/lease failure (id Software win_glimp.c GLW_SetMode, fallbacks to windowed mode and r_mode, error paths, message boxes). Find where the system deadlocks or spins: kernel lock, IRQ-off loop, display presenter, Win32 broker, desktop. Use the H1 Quake III harness where it helps.
2. **Fix it.** A failing full-screen lease returns the documented Windows error (DISP_CHANGE_FAILED etc.) and the game continues windowed or exits cleanly, with the desktop and other programs responsive. Any wait in the display, lease or presenter paths is bounded. No new panic paths.
3. **Installer.**
   - Check what VISE does when it launches the game at the end (CreateProcess, then WaitForInputIdle or another wait). Implement WaitForInputIdle with Windows semantics: it returns when the child's GUI thread waits for input with nothing pending, on timeout, or WAIT_FAILED for console/non-GUI children. A hung or failing child must never block the parent's message pump forever.
   - Check the desktop's not-responding handling: after "Wait", the dialog must not re-open immediately in a loop. Windows ghosting re-checks only after the 5 s hung-app timeout. The ghost window must keep moving and closing.
4. **Tests.** Host reproductions of both problems, before and after the fix. Existing Win32, desktop, display and render3d suites stay green. Run one at a time with nice -n 19.

Write the exact QEMU reproduction the lead should run (injected lease failure through an existing test hook, or a new runtime-only test selection, without build variants) in build/codex/fx-q3hang-report.md. Leave changes uncommitted.

## Lead amendment 1 (2026-10-26 21:46): review round 1

Committed as f9786b22e. A Codex xhigh review found one P1 and three P2 (full text build/codex/fx-q3hang-review1.md); fix all with regressions:
1. P1 same-process dialogs can leave the screen frozen: apps/desktop/input.c:117 retains the lease for every same-PID activation including a composed MessageBox or error console; composition stays suspended and the first-frame watchdog no longer applies (display_present.c:266). Restrict retention to the carrier-to-drawable handoff; any other same-process window must resume composition.
2. P2 releasing a borrowed lease does not release the carrier's grant (sdk/win32/user32/display.c:179/:216): track the borrower's relationship to the carrier and revoke synchronously on GL teardown or transition to composed presentation.
3. P2 WaitForInputIdle can wait forever for a GUI-subsystem process that never creates a message queue (personality.c:77, message.c:229): follow Microsoft's documented behaviour (immediate return / WAIT_FAILED as documented) based on runtime queue presence.
4. P2 the interactive QEMU reproduction paints the evidence slideshow over the desktop (f5_probes_observers.c:35 -> probes.c:1140 show_evidence_forever): give that selection a sleeping continuation.
Also use DISP_CHANGE_FAILED (-1) where the driver failed, per the reviewer's note. Report as "Review round 1". Leave changes uncommitted.
