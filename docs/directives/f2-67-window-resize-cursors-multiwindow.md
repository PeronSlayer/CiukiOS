# f2-67 — Desktop refinement: window resizing, complete cursor set, multi-window behaviour (owner request 2026-10-15)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-67` from `main`. Host-testable; the lead reviews PNGs and the T23.

## Request

The owner judged the window resizing on the T23 unacceptable and asked that a refinement pass re-check every detail of the desktop's interaction, not only the items listed. This directive covers three areas; f2-68 covers text selection and the paint program. Documents and comments in plain technical prose, no names of other operating systems.

## Task

1. **Audit first.** Write `docs/design/desktop-interaction-audit.md`: every window interaction and every widget of the desktop and its applications with its current behaviour, verified by running the host replay harness and reading the goldens, marked ok / wrong / missing. The lead reads it before the implementation starts (deliver it as milestone A within the first hour, then continue).
2. **Window resizing.** Resize from every edge and corner with the matching cursor; live feedback (outline or live content, consistent, no tearing, no stale strips); minimum and maximum sizes honoured; the content relayouts on release and during live resize for the native applications; keyboard resize through the system menu; maximize/restore keeps the previous geometry; double-click on the title bar maximizes and restores.
3. **Cursor set.** Arrow, busy (hourglass) and arrow-with-busy, I-beam over text fields, not-allowed, four resize shapes (horizontal, vertical, two diagonals), move, link hand, crosshair, precision select; drawn in the theme's style at 32×32 with hot spots; the kernel/desktop cursor path switches shape per hit test and per application request (the desktop protocol gains a cursor request message if it has none); hardware cursor path (f1 Savage) and software path both.
4. **Multi-window behaviour.** Activation and z-order rules on click, on launch and on close (the next window of the same application, then the previous top); active and inactive title bars; taskbar buttons mirror the state; minimize, restore and maximize with the classic outline animation; new windows cascade; modal dialogs centre on their owner and block it; Alt+Tab list with icons, Alt+F4, Alt+Space system menu, Alt+Esc; drag beyond the screen edges keeps the title bar reachable.
5. Goldens and PNGs: re-record affected goldens; before/after PNGs under `build/host/f2-67/` for each item.

## Acceptance

Audit document delivered; desktop host suites and gate green with re-recorded goldens; the lead checks the PNGs and the owner judges on the T23. Files: `apps/desktop/*`, `apps/libdeskui/*`, the demo apps as needed, `assets/` for cursors (own drawings or Tango-derived, licence noted).

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f2-67-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
