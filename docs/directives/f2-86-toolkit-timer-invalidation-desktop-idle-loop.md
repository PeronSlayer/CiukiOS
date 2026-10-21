# f2-86 — libdeskui timers must not invalidate the window; desktop idle loop at 1 ms; file-dialog places poll

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead from the T23 trials of images `3d18f2db…` and `5b74e640…` (Files at 50–65 % user CPU while idle-open, the desktop at 20–30 %; QEMU shows the same structure at 16 % / 46 %) and a read-only Codex review (`build/lead-scratch/idle-cpu-review.md`, kept in the report)
- **Worktree:** `../CiukiOS-wt-f2-86`, branch `wt/f2-86`

## Findings (verified in the tree)

1. `apps/libdeskui/widgets.c:88` `du_emit()` sets `u->dirty = 1` (full window) for **every** event, including `DU_EV_TICK` delivered at `widgets.c:288`. Files arms `T_BROWSER` every 20 ms (`apps/files/app.c:1635`) and `T_MEDIA` every second: up to 50 full `du_render()` passes and 50 whole-back-buffer comparisons per second (`client.c:254`, `client.c:141`) on a 760×520 window while nothing changes: about 20 M pixels cleared and 160 MB/s compared per second. `DESK_DAMAGE` is suppressed when pixels are equal, so the cost is pure CPU in the client: on the T23 this is the Files lag and the system-wide slowdown the owner reported.
2. `apps/desktop/desktop.c:1943`: the main loop sleeps **1 ms**, not the 10 ms the comment above the loop promises: about 1,000 service turns per second (heartbeat/client scans, clock reads, shell checks, worker mailboxes, one empty nonblocking receive per client, display-change counter, Win32 fault/service polls in `win32_native.c:431`).
3. `apps/libdeskui/dialog.c:64`: the file-dialog places poll rebuilds its list and marks the UI dirty every 500 ms even when unchanged.
4. `apps/files/app.c:1149` `job_tick`: the timer's prior `dirty = 1` defeats `du_progress()`'s dialog-only painting during a job.

## Required

1. **Timer events do not invalidate.** `du_emit()` leaves `dirty` untouched for `DU_EV_TICK`; callbacks invalidate what they change (`files_update`, `du_progress`, explicit `du_invalidate`). Every existing behaviour that relied on the implicit repaint (status text, progress dialogs, the DOS Box settings panel if it uses the toolkit, CiukNote/Paint/Music/Terminal timers) is checked and given an explicit invalidation of the changed bounds. The Files browser poll stops while the worker is completely idle and restarts on submission (secondary saving, keep the 20 ms cadence while a scan is outstanding).
2. **Desktop idle sleep.** The main-loop pause becomes deadline-based: at least 10 ms when no input is pending, no animation, no damage and no worker result is due; shorter only until the next real deadline (frame due, animation step, timer, heartbeat); never longer than 10 ms with input pending. The per-turn service polls (clients' empty receives, Win32 fault/service polls, display counter) run at most once per 10 ms when idle. Interaction latency must not regress: the existing input-lag records (`[display] input_lag_us`) and the desktop gate tests bound it.
3. **Places poll compares before invalidating** (`dialog.c:64`): rebuild into a scratch list, invalidate only on a difference.
4. **Tests.** `tests/host/desktop` (`deskui_test.c`: a window with a repeating timer and an idle callback renders zero times per second and compares zero rows; a callback that changes a widget renders once with that widget's bounds; the progress dialog repaints only the dialog); the Files tests (`test_desktop.py` Files cases, `test_desktop_folder.py`); desktop loop tests (`desktop_test.c`/gate: an idle desktop performs ≤ 100 service turns per second and keeps input latency ≤ the current bound); the dialog places test. `make CAP= apps` once, `python3 -B -m unittest discover -s tests/host/desktop -t .` as far as the sandbox allows, `(cd tests/host && python3 -B -m unittest test_desktop_gate)`.

## Constraints

Behaviour-preserving (no visual change); UI copy untouched; `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; English. Report `build/codex/f2-86-report.md` with before/after render counts from the host tests.
