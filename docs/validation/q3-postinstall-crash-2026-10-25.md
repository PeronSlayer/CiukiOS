# Quake III demo: desktop death, w32srv restart storm and kernel panic (2026-10-25)

Image under test: main `c4061850d` (QEMU, `qemu-desktop-2002` twin). Worktree
of the fixes: `wt/q3crash` on main `ea5a23277` (includes the fx-uafind AVL
extents and the f7-savage render3d merges).

Evidence: serial log `build/lead-scratch/q3play-serial-c406.log` (installer
run, 6042 lines), serial log `build/lead-scratch/q3run-clean-serial.log`
(quake3.exe on a clean system, 544 lines), the disk after the installer run
(`build/lead-scratch/q3crash-disk/disk-standalone.qcow2`, FAT32 partition at
offset 1048576), and the lead's gdb backtrace of the halted CPU.

## Timeline (installer run)

| Uptime | Event (serial) | Meaning |
| --- | --- | --- |
| 9.8 s | `[launch-stage] name=w32srv pid=3` | desktop pid 2 spawns its window server |
| ~130 s | `Q3ADemo.exe` pid 6 runs (VISE installer), 10 min copy | installer serves its windows through the desktop (service 1) and w32srv (service 2) |
| 741.0 s | `[win32] exit pid=6 code=00000000 reason=normal` | installer exits normally |
| 741.0 s | `[supervisor] desktop died pid=2 status=256 deaths=1; restarting` | desktop `main()` returned 1 (`status=256` is exit code 1), no desktop line before it |
| 741.1 s | `[supervisor] desktop restarted pid=7`, `w32srv pid=8` | new desktop; its w32srv exits at once |
| 741.5–776.7 s | 323 × `[win32] w32srv restart pid=N elapsed_ms=0` | the desktop respawns w32srv without a pause |
| 771.1 s | `[win32] start pid=284 exe=/Q3Ademo/quake3.exe` | quake3.exe launched |
| 776.7 s | `Ciuki VMM panic ... reason=sync:_blocking_operation_outside_thread_context` | kernel panic in task quake3.exe |

The disk shows `C:\WINDOWS\SERVICE.CRS.0/1` (the w32srv window/atom
directory) written at guest time 21:32, i.e. 771 s after the 21:19 boot, with
quake3.exe's windows (`Quake 3: Arena`, `Quake 3 Console`, pid 0x11c = 284):
a w32srv was serving quake3 while every *new* w32srv died instantly. That
server was the dead desktop's orphan, pid 3. Both slots parse on the host
(`w32srv --seed` of the extracted hives exits 0); the
`registry directory-invalid-slot fallback` lines are the benign mid-write
read of a slot pid 3 was rewriting, not the cause of the deaths.

## 3. Kernel panic: `sync: blocking operation outside thread context`

Backtrace (lead, gdb on `VMM.ELF`): `fatal_screen` ← `panic` ←
`thread_context()` (core/sync.c:360) ← `wait_until` ← `kwait_wait_until` ←
`w32_user_return` (proc/win32/apc.c:166) ← `proc_signal_return_to_user`
(proc/signal.c:308) ← `trap_return`.

Cause. `w32_user_return` parks a suspended Win32 thread (SuspendThread,
CREATE_SUSPENDED) with `kwait_wait_until` on the user-return hook. That hook
runs on `trap_return` with IF cleared: every IDT entry is an interrupt gate
(`arch/desc.c`), the syscall path does `sti()`…`cli()` around the dispatch
(`arch/trap.c`), a timer IRQ that preempted the thread returns through the
same tail, and so does a new thread's first activation (`task_first_entry`).
`thread_context()` panics when IF is off, so *any* suspend that took effect
at user return panicked the kernel; the host harness never modelled IF at the
hook (its `kwait_wait_until` stub had no thread-context rule), which is why
the existing suspend test passed. quake3.exe imports `CreateThread` and is
the first threaded Win32 title with a suspend on this path.

Fix (`src/kernel/proc/win32/apc.c`): the park is `park_until_resumed()` —
`T_BLOCKED` + `schedule()` with a one-tick wake timer as safety net, the way
`arch/trap.c` ("still inside the IF-disabled interval") and the NDIS user
host park at user return; `w32_unpark()` makes the parked task runnable from
`ResumeThread` (TerminateThread already wakes it through `proc_interrupt`).
No blocking wait runs on the hook any more.

Host evidence (`tests/host/win32/kernel_objects_test.inc`, kernel core suite):
the harness now mirrors `thread_context()` in its `kwait`/`kmutex` stubs and
lets `schedule()` accept IF off as the kernel does (`tests/host/proc/proc_test.c`).
New checks: SuspendThread then `w32_user_return` with IF cleared (the trap
tail) — the thread parks blocked with the one-tick timer, the resume makes it
ready at once, the context is restored; the same with the timer firing first;
`w32_unpark` on a non-parked thread is a no-op. Before the fix the suite fails
at the thread-context rule (`kernel_core_test.c:65`); after it, `OK`
(5 tests). The pre-existing `test_shared_tick` assertion `old_steps>=5*1500`
(linear extent walks) was already failing on main `ea5a23277` after the AVL
merge and is restated as a bound.

## 1. Desktop exit with status 1 when the installer exited

Cause. When a Win32 client dies, `w32_services_cleanup()`
(`src/kernel/proc/win32/service.c`) closed **the server's** end of the
service link from the kernel (`desktop_close(l->server, l->server_fd)`); for
service 1 the server is the desktop and that fd is `d->clients[i].channel`.
The desktop's turn reads channels first (a closed fd there is dropped) and
polls last; a close that lands in between (the exit was collected while the
desktop rendered the installer's closing windows) makes `poll()` report
`POLLNVAL`, and `desktop.c` returned 1 on any `POLLNVAL`, silently. Closing a
number under a `poll()` server can also hit a *reused* descriptor (a worker's
directory) which `desk_drop_client` would then close.

Fix. Kernel: the server's end is never closed under a polling server; the
server reads the hang-up (`recv` returns 0, `channel_poll_events` has
`POLLHUP`) and closes it itself, as the desktop and w32srv already do.
Desktop (`apps/desktop/desktop.c`, `client.c`, `desktop.h`): `POLLNVAL` on a
client slot drops that client through `desk_client_invalid()` without
`close()` (`channel_invalid`); only the desktop's own descriptors (input,
completion) stay fatal, and every `return 1` of the main loop now writes
`desktop: exit: ...` to stderr.

Host evidence: kernel core suite — after a Win32 client's death the server's
fd object is kept, reports `POLLHUP`, and `desktop_close` by the server
frees it (fails before the fix at `kernel_objects_test.inc:240`). Desktop
lifecycle test (`tests/host/desktop/desktop_test.c`,
`DesktopTests.test_native_production_model`): `desk_client_invalid` drops the
window and its surface and leaves the channel number alone; an empty slot is
ignored. `desktop.c`, `client.c`, `win32_service.c` and `w32srv/main.c`
compile with the installed SDK compiler (`-fsyntax-only`, `-Werror`).

## 2. w32srv restart storm (323 restarts)

Cause. The desktop's death left its w32srv (pid 3) alive: orphans are adopted
by PID 1 (`proc_collect`), and pid 3 kept the Win32 service registration.
`W32_SVC_REGISTER` refuses a new server while a live one is registered
(error 5), so every w32srv of the new desktop exited with code 1 — silently
(`main.c` had no log on that path) — and `desk_w32_supervise` respawned it
with `retry_ns = now`. Not a registry-store corruption: both `SERVICE.CRS`
slots load on the host; the "invalid-slot fallback" is a benign torn read of
pid 3's rewrite.

Fix. Kernel (`service.c`): a registration whose holder is stale (a window
server whose parent is no longer the supervisor's desktop, or a desktop PID 1
no longer runs) retires the holder (`proc_stop`) and succeeds, with a
`[win32] service server ... stale registration; stopped` line; a refusal is
logged with the holder's pid. `w32_services_desktop_died()` is called by the
supervisor when the desktop dies (`src/kernel/proc/supervisor.c`) and retires
the registered window server at once. w32srv (`apps/w32srv/main.c`) says why
it did not start (`[win32] w32srv start-failed stage=register error=5`,
flush, defaults, reconcile) through the console log. Desktop
(`apps/desktop/win32_service.c`): an exit with a nonzero code within 2 s of
the start, or a fourth rapid death of any kind, defers the next spawn 250 ms,
500 ms, 1 s, 2 s, 4 s, then 5 s (`[win32] w32srv restart deferred`); a crash
of a server that has been serving keeps the immediate restart (f5-09b).

Host evidence: kernel core suite — the dead desktop's orphan is still live and
registered; the next desktop's w32srv registers successfully and the orphan is
stopped; a live holder under the current desktop still refuses a rival
(error 5); the supervisor hook retires the registered server. Adapter suite
(`tests/host/desktop/win32_adapter_test.c`, `test_win32_adapter.py`, 4 tests
OK): refused starts are deferred 250 ms → 5 s, no spawn before the deadline,
a crash after 3 s of service restarts at once and resets the count, three
early crashes restart at once, the fourth is deferred; the real-w32srv
supervision test (crash/SIGKILL, reconnect, flush, orderly shutdown) stays green.

## Suites run (host, `systemd-run` 3 GiB cap, one at a time)

- `tests/host/win32/test_kernel_core.py`: OK (fails before each fix at the
  stated lines).
- `tests/host/desktop/test_win32_adapter.py`: OK (4 tests).
- `tests/host/desktop/test_desktop.py DesktopTests.test_native_production_model`: OK.
- `build/host/{proc_test,uaddr_index_test,launch_test}` (include the changed
  harness): PASS.
- `scripts/test/host_kernel_tests.sh` (whole runner, from the worktree):
  every C harness passed (proc_test, signal_test, desktop_test, files_test,
  kernel core, display suites); the one error is
  `test_kernel_loader.test_builtin_image_payload_rules_without_image_build`,
  which needs the qualified Wine upper-DLL cache (`advpack.dll`, "run make
  upper") that this fresh worktree does not have — unrelated to the change.
  The `probe=crash-isolation ... status=FAIL reason=desktop_contract` records
  in that log are the desktop_test fixture's negative cases under fake
  scheduling (the program ends `desktop final ... PASS`); the same records
  are in the lead's earlier runner logs (`build/codex/f1-52b-kernel-host.log`,
  `f2-51b-host-kernel.log`).

## Codex review of wt/q3crash (2026-10-26): findings and fixes

The review (`build/lead-scratch/q3crash-review.md`, gpt-6.1-sol xhigh) found
no blocker in the parking fix and four defects in the rest; all fixed, each
with a host test that fails on the committed `c2a784bc4` and passes now.

- **P1 — stale-server retirement used the process pointer across a record.**
  `klog` may sleep on serial backpressure (`core/serial.c`); the old order
  logged, then `proc_stop(server)` and published. During the record the old
  server could be reaped or a rival could register. Now lookup, stale check,
  `proc_stop` and publication are one non-yielding interval and the record
  is written afterwards from saved scalars (`service.c`:
  `server_retired_log`, REGISTER, `w32_services_desktop_died`). Test: the
  harness `klog` runs a hook; a rival registering during the record is
  refused (error 5) and the first registrar stays; the retired server
  exiting and being reaped during the record is harmless. On the old code
  the rival registered (`kernel_objects_test.inc:298`).
- **P2 — descriptors of connections lost before ACCEPT leaked.** The
  cleanup kept every server end; an unaccepted link's server fd (installed
  by `channel_connect`) is never reported by ACCEPT, so the desktop and
  w32srv could not reclaim it (one per failed launch, 128-entry table). Now
  only accepted links keep their server end; an unaccepted one is closed
  with the object-identity check. Test: five Win32 programs dying before
  acceptance leave the desktop's and w32srv's fd counts unchanged.
- **P2 — desktop replacement discarded surviving clients' identities.**
  Every new desktop started its first w32srv with `--fresh-clients`, whose
  `wr_clients_reset` removes all process-owned atoms and HWNDs, also those
  of programs that survived the desktop's death; the earlier claim that
  they "reconnect as after any w32srv restart" was too strong. Now
  `desk_w32_fresh_clients()` (`apps/desktop/win32_service.c`, used by
  `spawn_service`) passes the flag only on a cold boot. Re-review
  (2026-10-26): the first version compared `start_tick`s strictly, but
  publication records the millisecond tick (`process.c proc_publish`), so a
  survivor and the replacement desktop could share it and the survivor lost
  its identities; a failed snapshot also chose the reset. Now the decision
  is by PID order, which is monotonic within a boot: a live or stopping
  process with `1 < pid < desktop pid` is a survivor (replacement, no
  reset); none means a cold boot; a NULL/invalid snapshot or a failed
  `ciuki_proc_list` chooses the non-destructive reconcile (`wr_reconcile`
  against the live processes keeps surviving owners). Tests: the decision
  function (cold boot; older PID at the same, an earlier and a later tick;
  stopping; zombie; newer PID; desktop absent; invalid version/NULL/pid 1
  snapshot → keep); the registry harness (`registry_test.c reconciliation`):
  a live owner's HWND and local atom keep their values across a server
  restart without the reset and are removed by `wr_clients_reset`. Limit: a
  boot-time service with a PID below the desktop's would also read as a
  replacement; none exists today (the desktop is PID 2 in both logs).
- **P3 — fatal `poll()` errors returned 1 silently.** `desktop.c` now logs
  `desktop: exit: poll errno=N` before returning.

Suites after the review fixes: kernel core OK (fails on HEAD `service.c`
at `kernel_objects_test.inc:298`), `test_win32_adapter.py` OK (4 tests,
incl. the lifecycle fixture's `--fresh-clients` on a cold-boot snapshot),
`test_registry.py` OK (16 tests); `desktop.c`, `win32_native.c`,
`win32_service.c`, `client.c` compile with the SDK compiler.

## Quake III console: every other keystroke lost (2026-10-26)

Evidence (lead, QEMU, main `fa6d33a53`): with one key per 0.4 s (press 40 ms,
release, 400 ms gap) the console received "ma 3m7" for "map q3dm17"; with
the QA's faster typing one character per line was lost. Quake III 1.11 reads
the keyboard through WM_KEYDOWN/WM_KEYUP/WM_CHAR (win_wndproc.c; its
DirectInput device is the mouse only, win_input.c) and pumps with
PeekMessage(PM_NOREMOVE), GetMessage, TranslateMessage, DispatchMessage per
message (win_main.c Sys_GetEvent).

Reproduction on the host with the real binary: `tests/host/win32/installers/
test_quake3.py test_console_typing_through_window_messages` runs quake3.exe
on the production DLLs under the H1 stand-in desktop (`h1_game.c`), which
types `+set ciuki_keys "text"` on the lease frames the way the kernel
delivers keys (KEY down with its TEXT, KEY up; `ciuki_keys_gap` idle frames
between keys, `ciuki_keys_burst 1` for a whole line in one frame) and reads
the console's `]\text` echo from qconsole.log. Before the fix, gap 0 typed
"echo ciuki typing q3m17 demo001" (the `d` lost); the stand-in's `GAME post`
trace showed WM_KEYUP posted before the same key's WM_KEYDOWN and no WM_CHAR,
right after a `DESK_INPUT_SETTINGS` request.

Cause (`sdk/win32/user32/input.c`): `u32_input()` began with
`u32_settings()`, whose `u32_input_refresh()` issues a `DESK_INPUT_SETTINGS`
wire request once a second; `u32_wire()` waits by pumping the desktop
channel, so the TEXT and KEY up of the key being processed were handled
first (WM_KEYUP posted, the WM_CHAR left pending under the *previous* key's
time, never matched by TranslateMessage) and the WM_KEYDOWN was posted last.
At 0.4 s per key the one-second refresh fell inside every other key down.

Fix: no wire round trip or file read inside an input event
(`u32_input_depth`; `u32_settings()` and `u32_input_refresh()` return while
it is set — `GetDoubleClickTime()` in the button branch had the same
hazard); the pump refreshes the settings before taking a packet
(`u32_pump`). Also `u32_pump` now handles the events another thread deferred
to the window's thread before draining new packets (FIFO; a unit test
reorders one key down through the second thread and fails on the old order).
The stand-in kernel queue of h1_game.c now returns posted messages before
hardware input as `proc/win32/message.c` does (otherwise a burst of keys put
Enter's WM_KEYDOWN before the characters, a fixture artefact).

Host evidence: the real-binary test passes at gap 3, gap 0 and burst
(fails before at gap 0); `tests/host/win32/user32/test_user32.py` OK with the
Quake-shaped pump tests (six arrival points of the next key, a key down
deferred by a second thread, the one-second refresh firing during typing).
The lead must check in QEMU: "map q3dm17" and "demo demo001" typed at 0.4 s
per key and at the QA's speed arrive complete in the console.

### Codex review of wt/q3crash2 (two P2, 2026-10-26) — fixed

- The deferred-event FIFO of `u32_pump` drained a snapshotted count before
  new packets; a deferred `DESK_CONFIGURE` runs `WM_SIZE`, which releases
  the USER lock, so a key down deferred by another thread during it was
  overtaken by its TEXT and key up. Now `deferred_drain()` runs before every
  new packet and stops a pass only when an event deferred itself again (its
  window still creating, `u32_self_deferred`). Test: a deferred CONFIGURE
  whose `WM_SIZE` lets the second thread defer `d`'s key down, TEXT and key
  up arriving after; `d` is typed (fails before).
- `u32_settings()` set its re-entry guard before the 32 KiB allocation and
  left it set on failure, disabling the refresh for the process. The guard
  is now set only after the allocation and cleared on every exit. Test: the
  USER heap budget nearly exhausted, a due refresh fails, memory freed, the
  next due refresh issues `DESK_INPUT_SETTINGS` (fails before).
- Suites: `test_user32.py`, `test_input.py`, `test_dialogs.py` (20) OK; the
  real-binary console typing test at three pacings OK.

## What the lead must check in QEMU

1. `quake3.exe` on a clean image: no panic; the game reaches the menu and
   runs; serial shows no `sync:` reason. SuspendThread/ResumeThread now work
   for the first time on the real kernel — watch for a thread that stays
   parked (a resume lost): `[win32]` records and the Task Manager thread
   states.
2. Installer run to the Finish page and exit: the desktop survives (`[win32]
   exit pid=N` with no `[supervisor] desktop died`); no `desktop: exit:`
   line on the console.
3. Kill the desktop deliberately (or let a crash restart it): one
   `[win32] service server pid=... desktop died; stopped` line, the new
   desktop's w32srv registers (no `w32srv restart` burst) without
   `--fresh-clients`, and a Win32 program that survived the desktop keeps
   its windows and atoms (same HWND values, no identity loss).
4. If a w32srv start is refused for another reason, the console shows
   `[win32] w32srv start-failed stage=...` and `[win32] w32srv restart
   deferred` with growing delays instead of hundreds of restarts.

## Risks

- `park_until_resumed` schedules from the IF-disabled trap tail; this is the
  path `arch/trap.c` already uses for preemption, but it is the first Win32
  use. The one-tick timer bounds any lost wake.
- The kernel no longer closes a server's link fd on a client's death; a
  server that never reads its channels would keep the fd until its own
  death (the desktop and w32srv both read and drop on hang-up).
- Retiring a stale registered server stops a w32srv that may still serve
  Win32 programs of the previous desktop session; they reconnect to the new
  server, which keeps their persisted identities (no `--fresh-clients` on a
  replacement).
- `tests/host/proc/proc_test.c`'s `schedule()` no longer asserts IF on; the
  thread-context rule moved to the blocking stubs of `kernel_core_test.c`.
  Harness programs that only include proc_test.c lose that guard.
