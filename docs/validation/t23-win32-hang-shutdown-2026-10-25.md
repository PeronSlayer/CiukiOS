# Quake III installers: slow start, "Not responding", no close, no shut down (T23, image c42f556e)

Host investigation, 2026-10-25, of the owner's real-hardware report on the
image of main a347ca76b (hardware image c42f556e): opening the Quake III Arena
demo installer was very slow; with several instances running they kept
going "Not responding"; closing them did nothing; Shut Down and Restart did
nothing and the machine had to be switched off. No QEMU, image build or
hardware run was made for this record. Evidence: the T23 disk log
`LOGS/BOOT.LOG` of that session (6602 lines, kept privately); only record
lines are quoted. Times are seconds since boot, taken from the records that
carry them (`[launch-stage] spawn_us/elf_us`, the `[mem] ledger ... tick=`
snapshots, in ms); a line between two snapshots is dated by them.

The image did not yet contain the q3exec2 and q3exec3 fixes
(`docs/design/win32-personality-contract.md`); they explain the lost wizard
pages of the first instance and are not repeated here.

## Timeline

### Launches (desktop folder shortcut, `shell-launch` helper, Win32 loader)

| PID | `shell-launch` spawn | Q3ADemo spawn → loaded | loader read | engine loaded (`CreateActCtxW` record) |
| --- | --- | --- | --- | --- |
| 11 | 168.657 | 168.668 → 168.698 (30 ms) | 164,864 bytes, 163,840 from disk in 5 requests | before 173.47 (≤ 4.8 s) |
| 13 | 173.469 | 173.475 → 173.536 (61 ms) | 164,864 bytes, 0 from disk | 183.28–190.50 (9.7–17.0 s) |
| 15 | 176.626 | 176.648 → 176.746 (98 ms) | 164,864 bytes, 0 from disk | 190.50–199.79 (13.8–23.0 s) |
| 17 | 180.816 | 180.852 → 180.924 (72 ms) | 164,864 bytes, 0 from disk | 190.50–199.79 (9.6–18.9 s) |

- `[launch-stage] name=Q3ADemo.exe pid=11 spawn_us=168668000 elf_us=168698000 read_bytes=164864 disk_bytes=163840 disk_requests=5 dma_ms=0`
  and the matching lines for 13, 15, 17: the q3exec loader view (headers,
  sections, no 48 MB overlay) is read, hashed and mapped in 30–98 ms. The
  loader is not the slow part.
- The milestone column is the first `[win32] stub kernel32.dll!CreateActCtxW pid=N`
  of each process. Neither `Q3ADemo.exe` nor `vise32ex.dll` names that
  function; it comes from the Wine-derived `comdlg32.dll` DllMain
  (`cdlg32.c`), which runs when `LoadLibraryA("vise32ex.dll")` loads the
  engine's imports, after the stub has inflated `vise32ex.dll` (215,548 →
  495,616 bytes, 32 KiB `_hread`/`_hwrite` chunks, host disassembly) into
  `C:\WINDOWS\TEMP\~visNNNN`. The first instance reached it in under 4.8 s;
  the three that ran in parallel needed 10–23 s for the same work.
- The owner clicked again after 4.8, 3.2 and 4.2 s (four launches in 12.2 s):
  no window had appeared yet. The image records no first-window time for
  programs started through `shell-launch` (the desktop's `first-frame`
  record covers its own launches only).

### Load while the installers ran

- CPU (cpustat 2 s windows, per mille of the window): `idle_pm` 0–10 in
  every window from 183 s to 362 s. `device-work#9` used 165–342 ‰, all in
  kernel mode, with no DOS VM running (`device_overrun task=device-work#9`
  up to `count=315`). One installer thread used up to 391 ‰
  (`top=Q3ADemo.#76:391k202s0,...` at about 245 s); `Q3ADemo.#82` spent 189
  of its 213 ‰ in kernel mode (`top=Q3ADemo.#82:213k189s1,...`);
  `w32srv#40` used 68–119 ‰, most of it in kernel mode.
- Storage: 32 `[storage] vfs_hold drive=C ... task=Q3ADemo.exe#NN op=read`
  records of 50–401 ms with up to 3 waiters, between 170 s and 393 s;
  `[sched] inherit` records show the installers, `w32srv#40` and the
  desktop's worker threads `#41/#43/#44/#51` waiting for each other's drive
  and cache holds.
- 14 `[sched] donation_cycle` records between the installer threads
  (`#78`, `#80`, `#82`) and the desktop workers `#43`/`#44`, from 272 s to
  the end of the log (`waiter=78 owner=44` at 402–403 s). The kernel then
  donates nothing and relies on the waiter's request deadline; no storage
  timeout was recorded.
- The desktop's own loop stayed live: its watchdog recorded one stall in
  the whole period (`[desktop] stall stage=render ... no_frame_ms=730` at
  about 250 s) and none after the Restart request.

### The first instance

- 331.9–337.5 s: `[desktop] v3 drop pid=11 sender=11 opcode=12 length=20 fds=0 window=0 generation=3`
  (the q3exec3 PONG for window 0): every window of PID 11 was removed.
- 349.3–352.6 s: `[win32] warning check=dialogbox-create-failed module=user32.dll pid=11 tid=30`
  (q3exec2: VISE takes the failed page as Next).
- By 362 s its task (`Q3ADemo.#76`) and its `shell-launch` (`#75`) are no
  longer in the `[cpu] task=` lists: PID 11 had ended. The image records no
  Win32 process exit.

### Restart

- 395.5–397.9 s: `[desktop] end-session begin action=1 programs=1`
  (Restart; one part only: the Win32 broker turn. No native window was
  open; installers 13, 15 and 17 were alive).
- 397.9–402.1 s: `[audio] transport start ... task=desktop`: the desktop
  played a sound, consistent with the warning sound of "This program is not
  responding" five seconds after the turn began (G3 rule for a Win32 turn
  whose USER thread does not fetch WM_QUERYENDSESSION).
- No later desktop record: no `end task dialog`, `end task pid=`,
  `end-session cancelled` or `end-session agreed`. The log ends with
  `[sched] donation_cycle waiter=80 owner=78` after the 403.35 s snapshot,
  5.5–7.9 s after the Restart request; `BOOT.LOG` publishes its size every
  5 s, so the last seconds before the forced power-off are missing. A
  Shut Down attempt, which the owner also reports, left no
  `end-session begin` record: either it fell in the lost tail, or it came
  while the Restart session ran (a new session is then ignored), or its
  dialog could not open (cause 4).

## Causes

Proven from the log and the code:

1. **Slow start: not the loader.** 30–98 ms per launch. The time goes to
   the stub inflating the engine and to the engine reading its 48 MB
   archive, on a CPU with no idle time (device work 17–34 % of it) and a
   disk shared by four readers: the same step took ≤ 4.8 s alone and
   10–23 s with four instances. No Win32 runtime cost visible in the log
   could be cut; the device-work load belongs to the device/scheduler owners.
2. **"Not responding" is the 5 s HungAppTimeout rule doing its job.** Each
   installer's UI thread did not pump messages for more than 5 s while it
   read (`[cpu]`, `vfs_hold`). Windows 98 would list them as not responding
   in Close Program too.
3. **Closing could not end a busy or refusing installer.**
   - user32 posted WM_CLOSE for the desktop's close button. Windows sends
     WM_SYSCOMMAND SC_CLOSE, which the VISE background ignores (it grays
     Close in its system menu); WM_CLOSE reached DefWindowProc and destroyed
     the background and its wizard page while the engine went on.
   - A window that answers PINGs between its busy periods never got "This
     program is not responding": the dialog needed the window to be marked
     unresponsive 5 s after the request, and a decline (the page ignores
     IDCANCEL) ended the request. Task Manager's End Task then only sent the
     close again; nothing could end such a program.
   - A second click on the close button of a "Closing..." window did
     nothing.
4. **Shut Down / Restart could stop without a way out.**
   - The desktop's 16 client slots were shared by program connections,
     program windows (hidden Win32 top-level windows included) and the
     desktop's own dialogs. Four installers showing their wizard (connection
     + background + page each), Files, two Notepads and NetSurf need 17;
     End Task, Shut Down and the shutdown progress window then fail to open,
     with no record (a session's End Task failure cancels the session, also
     with no visible message), and the installers' own pages fail, which
     VISE takes as Next. The image had no record for a full table, so the
     log cannot show whether it happened in this run (NetSurf ran from 357 s
     to 389 s, while the three installers were still open).
   - The Win32 turn waited without limit on a process that fetched its
     WM_QUERYENDSESSION and has no window on the desktop (PID 11's state
     after its connection drop): nothing could mark it unresponsive.
   - The power sequence only targeted the desktop's clients and children.
     A Win32 process without a desktop connection (PID 11 after the drop,
     or an installer whose windows are gone) kept its files open, call 73
     answered EBUSY ("Unknown storage holder (no client remains)") and Force,
     which sent SIGTERM to the known targets only, had nobody to end; Force
     never escalated to SIGKILL either.

Not proven: what happened in the last seconds of the Restart. The record
of the Not Responding dialog, if it opened, is in the lost tail of the log.
In this run PID 11 had already ended, so the windowless-process and
power-target gaps above were not what stopped this restart; they would have
stopped it with the first installer still copying files (as q3exec2 saw in
QEMU).

## Windows 98 behaviour (research)

- HungAppTimeout, `HKCU\Control Panel\Desktop`, default 5000 ms: "how long
  the system waits for user processes to end after the user clicks the End
  Task command button ... If this threshold is exceeded, the End Task dialog
  box appears, stating that the process did not respond"
  ([Microsoft](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-2000-server/cc978614(v=technet.10))).
- WaitToKillAppTimeout (20000 ms) and AutoEndTasks: at shut down a process
  that does not end gets the End Task dialog unless processes end
  automatically
  ([WaitToKillAppTimeout](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-2000-server/cc978624(v=technet.10)),
  [AutoEndTasks](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc736867(v=ws.10))).
- EndTask: WM_CLOSE first; forcing destroys the window when that fails
  ([Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-endtask)).
- Windows 98 SE at shut down, for a hidden notification window: "This
  program is not responding. It may be busy, waiting for a response from
  you, or it may have stopped running ... To close this program
  immediately, click End Task" with Cancel, End Task and Wait
  ([BleepingComputer](https://www.bleepingcomputer.com/forums/t/14143/windows-98se-shutdown-problems/)).
- Windows 98 Resource Kit ch. 25 (`wrkc25_4.htm`, `wrkc25_5.htm`): Ctrl+Alt+Del
  Close Program ends an unresponsive application without affecting others.
- WM_SYSCOMMAND: the close button sends SC_CLOSE, DefWindowProc carries it out
  ([Microsoft](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand)).

## Fixes

| Change | Files |
| --- | --- |
| End Task waits HungAppTimeout: a window of the program still open 5 s after End Task gets "This program is not responding", even if it answers PINGs or declined; one dialog per End Task; `[desktop] end task timeout pid=... window=...` | `apps/desktop/client.c` (`desk_task_end`, `desk_close_timeouts`, `desk_session_deadline`), `apps/desktop/desktop.h` (`end_task_ns`) |
| A second close request (title button, Alt+F4, caption icon, task button Close) on a "Closing..." window opens the dialog at once, unless the program answered `CLOSE_ACK LATER` and answers PINGs | `apps/desktop/client.c` (`desk_user_close`, `close_later`), `apps/desktop/input.c`, `apps/desktop/shell.c` |
| The desktop's close request reaches Win32 windows as WM_SYSCOMMAND SC_CLOSE; Close grayed in the system menu or `CS_NOCLOSE` declines at once; a window surviving SC_CLOSE declines | `sdk/win32/user32/desktop.c`, `sdk/win32/user32/messages.c`, `sdk/win32/user32/internal.h`; contract section "t23close" |
| Programs leave `DESK_SYSTEM_SLOTS` client slots for End Task, Shut Down, the shutdown progress and messages; refusals recorded (tiered in the second round below: 3 slots, Task Manager and application prompts admitted from the first) | `apps/desktop/desktop.h`, `apps/desktop/client.c` (`desk_add_client_tier`, `desk_slot_available`, v3 CREATE_WINDOW, `desk_free_slots`), `apps/desktop/shell.c` (`desk_internal_open`) |
| Win32 turn: a process that holds its query for 5 s with no window on the desktop is not responding; every window in turn is recorded (`[desktop] end-session win32 turn pid=... hwnd=... delivered=...`); a failed End Task dialog is recorded | `apps/desktop/client.c` (`session_hung`, `session_step`, `desk_end_task_open`) |
| Power sequence: every live Win32 process from the broker (`W32R_SHUTDOWN_LIST`) is a target (closed, listed as "Windows program (PID n)"); Force sends SIGKILL (the automatic SIGTERM is qualified in the second round: only after the broker reported WM_ENDSESSION done) | `apps/desktop/client.c` (`desk_power_begin`, `power_terminate`, `desk_power_force`), `apps/desktop/win32_native.c` (`w32_power_pids`), `apps/desktop/desktop.h` (`power_w32_pids`) |

Design record: `docs/design/desktop-shell.md` (G3, "Amendments after the T23
trial of image c42f556e") and `docs/design/win32-personality-contract.md`
("t23close").

## Second round: the Codex review of `63c964019` (2026-10-25)

The review (`gpt-6.1-sol`, read-only) found three P1 and four P2 gaps in
the first round. Each is closed below; each fix has a host test that
asserts the behaviour the review objected to is gone (the asserted
"before" is the first round's own behaviour, which its case 5 had asserted
as correct: SIGTERM 3 s after the sequence began).

| Finding | Fix | Test |
| --- | --- | --- |
| **P1-1** shutdown could block inside the kernel: the desktop called call 73 synchronously; `storage_shutdown` waited forever for `writer_running` (`mount.c`) and every drive reservation; `fs_port.h` lock waits repeat forever; SIGKILL lets a sleeping syscall unwind, so a deadlocked storage waiter survives Force | Kernel: every shutdown preflight wait (namespace, writer batch, drive reservations, boot log) is bounded by `storage_shutdown_wait_ms` (5 s) and a timeout is the existing `EBUSY` refusal with nothing changed (`mount.c` `shutdown_volumes`, `storage_sync`; `vfs.c` `vfs_reserve_request_locked`); new call 73 actions `CIUKI_POWER_FORCE_REBOOT` (9) / `FORCE_OFF` (10) run beside a blocked orderly call, flush at most 3 s through the bounded suspend-flush worker, latch UNCERTAIN, then reset or soft-off (`power.c` `power_force`; `abi.h`, `power.h`). Desktop: `power_request` runs call 73 on a worker thread (`EINPROGRESS` meanwhile, `desktop.c`), the loop stays live; after `DESK_POWER_CALL_NS` (15 s) the busy dialog says the shutdown waits for a disk operation and Force is the forced action; a second Force after SIGKILL left the kernel at EBUSY is the forced action too (`client.c` `desk_power_tick`, `desk_power_force`, `power_busy`; `power_force_request`). A volume the forced path leaves dirty is checked by ScanDisk at the next start (`boot_scandisk`, `FAT_RO_DIRTY`) | `tests/host/fs/test_storage_namespace.c` `bounded_shutdown_test` (a writer batch and a drive holder outliving 200 ms give EBUSY in 190–2000 ms; before, the first case never returned) and `cancelled_wait_test` (a task ended while blocked behind a C: holder leaves in 1 ms with `-VFS_ECANCELLED`, nothing reserved, the holder releases normally: the kernel's SIGKILL path, `interrupted` → request `cancelled`); `tests/host/power_runtime_test.c` `test_forced` (beside a blocked OFF, flush bound 3 s, UNCERTAIN, reset / soft-off / ENOTSUP, desktop only; before, actions 9/10 were EINVAL); `hang_close_test.h` case 10 (EINPROGRESS for 15 s, busy dialog, Force calls the forced restart); `shutdown_sequence_test.h` (the production `power_request` on its worker thread) |
| **P1-2** SIGTERM 3 s after the broker COMMIT could kill a program still saving in WM_ENDSESSION | After COMMIT the power sequence polls the broker STATUS (`power_w32_status`); a Win32 target gets its SIGTERM only once the broker reports `ENDED` (every WM_ENDSESSION answered, as Windows ends a process after its handler returns); not ended `DESK_WAIT_TO_KILL_NS` (20 s, WaitToKillAppTimeout) after the COMMIT, the sequence stops at "Some programs are still running ... have not finished ending their session" and Force is the explicit consent. Native targets keep the 3 s rule | `hang_close_test.h` case 6: 12 s of WM_ENDSESSION without SIGTERM, SIGTERM at ENDED; never ended: the dialog at 20 s, Force SIGKILL, a second Force is the forced restart |
| **P1-3** broker BEGIN/STATUS errors were taken as agreement | `session_broker_failed`: errors (EIO, or EBUSY while an ExitWindowsEx restart owns the broker) are retried for `DESK_SESSION_BROKER_NS` (5 s); past that the Win32 programs are *unknown*: no COMMIT, ordinary close of their windows, no automatic SIGTERM, busy dialog "The Windows programs could not be asked to save their work" with Force/Cancel; `[desktop] end-session agreed ... win32=committed\|unknown\|none` | `hang_close_test.h` case 7: BEGIN failing for 5 s (>10 retries) then unknown, no COMMIT, no SIGTERM, the user asked at the close deadline; a STATUS error that recovers within the window changes nothing (COMMIT once) |
| **P2-4** lossy Win32 inventory (capacity 32, enumeration failure → zero programs) | `DESK_POWER_TARGETS` 64 (`CIUKI_PROCESS_MAX`); `power_w32_pids` returns `-errno` (`-ENOSPC` on truncation, never a silent cut); a failed list is recorded, fetched again on every closing tick and before Force (`power_list_w32`), named in the busy dialog | `hang_close_test.h` case 5: the list fails at begin (error kept, one target), recovers at the next tick (two targets, the known one marked Win32), fetched again before Force |
| **P2-5** reserved slots rejected Task Manager; built-ins and message boxes could consume the reserve | `DESK_SYSTEM_SLOTS` 3 with tiers (`desk_slot_available`): programs and every ordinary built-in or message box need >3 free; the Task Manager the desktop launches itself is admitted at `DESK_SLOT_EMERGENCY` (`desk_add_client_tier`, `desktop.c` `launch_program`); End Task, Shut Down, the progress window and a message during a session/shutdown take the last two | `hang_close_test.h` case 1: 13 programs, About and a message box refused, Task Manager admitted, a second emergency connection refused, End Task dialog and Shut Down both open with the table full; `memory_footprint_test.h` fills to the reserve |
| **P2-6** real application save prompts refused with slots free (`DialogBox` returned -1) | An owned v3 window (DialogBox/MessageBox) of a process being closed (close request pending, the session's Win32 turn, or the power sequence) is admitted from the emergency reserve (`process_being_closed`, `[desktop] prompt admitted`); unowned windows and idle programs are still refused there | `hang_close_test.h` case 9: the idle program's prompt and an unowned window refused, the closing editor's "Save changes?" admitted, a second prompt refused, End Task still opens |
| **P2-7** a fetched query pumping forever without a prompt held the shutdown; w32srv shutdown unbounded | A prompt is a window of the process newer than the window-id watermark of the poll before the query was fetched (`w32_mark`/`w32_poll_mark`); without one a fetched query is bounded by `DESK_WAIT_TO_KILL_NS` (20 s), then "This program is not responding" (End Task / Cancel). `desk_w32_shutdown`: SIGKILL 10 s after SIGTERM, a reaped kill is a lost flush (recorded) not a failed shutdown, unreaped 10 s later it is abandoned (`DESK_W32_SHUTDOWN_MS`) | `hang_close_test.h` case 8 (pumping 20 s → dialog, Cancel keeps all; a prompt opened after the query waits 44 s without a dialog); `end_session_test.h` G3 Win32 (the prompt opened between two polls still counts); `tests/host/desktop/win32_native_lifecycle_test.c` (SIGKILL at +10 s, reaped kill → 1, unreaped → abandoned at +20 s) |

Research for the timeouts: the Windows 98 Resource Kit in `build/rskt`
(`wrkc25_5.htm`, "If an application stops responding, you can press
CTRL+ALT+DEL to display the Close Program dialog box, and then close the
unresponsive application without affecting other running tasks") describes
the Close Program behaviour but gives no timeout values; HungAppTimeout
(5000 ms) and WaitToKillAppTimeout (20000 ms) are the documented defaults
of the Windows 2000 registry values (links in the research section above),
which Windows 98 also keeps under `HKCU\Control Panel\Desktop`. The
review's point stands: these values are not proven Windows 98 figures; they
are the documented Windows defaults closest to it and are single constants
(`DESK_HUNG_NS`, `DESK_WAIT_TO_KILL_NS`) if the owner wants others.

### Second-round results (host, systemd scope MemoryMax=3G)

- Launcher harness (`tests/host/desktop/shell_launch_test.c`, the default
  run): shutdown sequence, G3 end-session (7 scenarios) and the ten T23 cases
  pass.
- `tests.host.desktop.test_win32_adapter`: 4 tests OK (the lifecycle test
  now covers the bounded w32srv shutdown).
- `tests.host.win32.user32.test_user32` and
  `tests.host.win32.dialogs.test_dialogs`: 23 tests OK (105 s).
- `tests/host/fs/test_storage_namespace.c` (host_kernel_tests.sh recipe):
  2227 checks, 0 failures, with `bounded_shutdown_test` and
  `cancelled_wait_test`.
- `tests/host/power_runtime_test.c` with `test_forced`: PASS.
- `tests/host/abi_layout_test.c` against the i686 dump: 2940 checks PASS
  (`CIUKI_POWER_FORCE_REBOOT` 9, `CIUKI_POWER_FORCE_OFF` 10 added).
- Compile-only checks: `power.c`, `mount.c`, `vfs.c` with the kernel's
  i686 flags; `client.c`, `shell.c`, `desktop.c`, `win32_native.c`,
  `input.c` with `ciuki-cc -Werror` (desktop flavour; `win32_native.c` also
  with the control flavour), against the worktree's `abi.h`.
- Full desktop suite (`python3 -m unittest discover -s tests/host/desktop
  -t .`, 1892 s): 224 tests, 5 skipped, one failure:
  `test_window_model` aborted in a fuzz run on a double close in the
  harness (`tests/host/desktop/window_model_test.c` closed a refused
  CREATE_WINDOW surface the desktop had already closed through
  `close_attachments`; the three-slot quota made that refusal reachable,
  its own guard still assumed two). Harness fixed (quota from
  `DESK_SYSTEM_SLOTS`, no second close); `test_window_model` rerun: 2 tests
  OK, `PASS fx-15 window model: 11565 frames identical to a full redraw`.
  No production code changed for it.

### Second-round risks

- The forced actions bypass the device quiesce and the durability barrier:
  a volume not flushed within the 3 s bound is left dirty on purpose
  (ScanDisk at the next start). The desktop reaches them only after the
  user pressed Force twice, or once on a call 73 that has not returned in
  15 s.
- `power_request` on a worker thread: a Cancel while the call runs leaves
  the worker to finish; the next sequence sees `EINPROGRESS` until it
  returns and then takes its result (the kernel refuses concurrent
  sequences with EBUSY anyway).
- Three reserved slots leave 13 to programs (16 - 3); the first round left
  14. Every Win32 top-level window still costs a slot (open point below).
- A Win32 program whose prompt was already open before the session began is
  not recognised as prompting (older than the watermark): after 20 s of
  pumping the user is asked, and Cancel keeps everything running.
- The w32srv abandonment path (not reaped 10 s after SIGKILL) proceeds to
  call 73 with the service's files possibly still open: EBUSY, the busy
  dialog, Force, then the forced action. It cannot hang.

## Third round: the second Codex review (xhigh, of `bc9881f62`, 2026-10-25)

The second review confirmed P1-2 closed and call-73 authority/ABI fine, and
found eight gaps (4 P1, 4 P2). Each is closed below with a host test whose
assertions the `bc9881f62` engine violates (the reviewer's sequences are the
cases).

| Finding | Fix (files) | Test |
| --- | --- | --- |
| **1 (P1)** a Win32 thread stuck in `fs_lock_take` survives SIGKILL, its broker connection stays open, the HOLD session has no deadline, End Task only reset `w32_since`: Force never appeared | `client.c`: End Task during the Win32 turn records `w32_killed_pid/ns`; no second dialog while its exit is awaited; a process the broker still reports `DESK_KILL_WAIT_NS` (5 s) later makes the Win32 part unknown (CANCEL best effort), the session finishes, the power sequence lists the process (not asked) and offers Force; the forced action follows the second Force, keeping the stuck process's resources | `hang_close_test.h` case 12: End Task at 6.1 s, no dialog until 11.0 s, unknown at 11.25 s, busy dialog at the close deadline, Force (SIGKILL again), "Force again", forced restart |
| **2 (P1)** `power_prepare` ran `fflush`/`fstat`/`close` (VFS locks) on the input thread; a failed `pthread_create` fell back to a synchronous call 73; a busy run/history worker suppressed every power tick | `desktop.c`: `power_prepare` closes observer channels and snapshots the descriptors to keep; `power_prepare_files` (fflush, fstat, close) runs on the power worker before the call; no synchronous fallback (`EAGAIN`, recorded); `run_history_idle` refuses with `EAGAIN` instead of gating the tick; the tick always runs during a sequence. `client.c`: `EAGAIN` retried like `EBUSY`, named by the busy dialog ("The desktop has not finished its own work"), Force then the forced action | case 16 (EAGAIN for 5 s → busy dialog → Force → "Force again" → forced power off); `shutdown_sequence_test.h` (descriptor cleanup through `power_prepare_files`, the production call on its worker) |
| **3 (P1)** Cancel restored the desktop while the detached worker continued call 73; its result could be taken by a later request | `client.c` `desk_power_cancelable`: false from the worker's start to its return; `shell.c` draws no Cancel then and ignores Esc/Enter/close box; `desk_power_cancel` refuses (`[desktop] power cancel refused`); `power_request(struct desktop *, action)` carries `power.serial` and `desktop.c` discards a result of another request (recorded) | case 13: Cancel and Esc refused while EINPROGRESS, allowed again after an EBUSY result |
| **4 (P1)** failed inventory + failed BEGIN left zero targets and the shutdown proceeded without consent | `client.c`: `w32_listed`; an unresolved inventory (`w32_list_error`, or `w32_unknown` without a successful list, or `targets_overflow`) stops at the busy dialog at the close deadline even with no target; Cancel or Force only | case 14: BEGIN and list failing → busy dialog ("could not be listed"), no call 73; Force proceeds. An empty successful list with BEGIN failing proceeds |
| **5 (P2)** `power_force` skipped device quiescence and published `power_source_mark_stopped`, the orderly proof APM trusts | `power.c`: a bounded (2 s) device fence through `power_source_quiesce_dma` (the platform's nonblocking native DMA/IRQ stop step); reset regardless; Off refused (`EBUSY`, power-button message) unless the fence proved every device stopped; then `power_source_mark_emergency_stopped` (`power_source.c/h`), a latch distinct from the orderly one that `power_source_soft_off`/`orderly_shutdown` accept | `power_runtime_test.c` `test_forced`: fence timing out → FORCE_REBOOT still resets after 3 s + 2 s, FORCE_OFF `EBUSY` with no soft-off and no latch; fence proved → emergency latch, orderly latch untouched, soft-off; no platform (safe mode) → `EBUSY` |
| **6 (P2)** emergency admission depended on allocation order (End Task first refused Task Manager; Task Manager first refused a prompt) | `desktop.h`/`client.c`/`shell.c`: `DESK_SYSTEM_SLOTS` 4, each slot records `slot_tier`, quotas per tier (programs 12, Task Manager 1, prompt 1, system 2), `desk_slot_occupancy`; Task Manager/prompt take a program slot while one is free | case 11 (both orders: End Task + Shut Down + Task Manager + prompt coexist, a fifth of any tier refused); cases 1 and 9 updated |
| **7 (P2)** any newer window counted as a prompt and disabled the 20 s escalation | user32 `desktop.c`/`dialog.c`/`internal.h`: a modal dialog with a live owner on the wire carries `DESK_V3_MODAL`; `client.c` `session_hung`: a prompt is newer than the watermark, modal, visible (not hidden/minimized) and owned by a window of the same process; explicit escalation (`session_escalate`): Alt+F4/close or End Task on a window of the waited program opens the session's dialog at once | case 18 (unowned progress window, non-modal owned window, hidden modal dialog: the dialog at 20 s); case 17 (escalation by Alt+F4 and by End Task); `end_session_test.h` G3 Win32 and cases 8/9 now model the prompt as an owned modal window |
| **8 (P2)** the 64-entry target table kept gone entries and dropped new live PIDs | `client.c` `power_target`: a full table reclaims an entry whose process is confirmed gone (`power_live`); none: `targets_overflow` (recorded), which is an unresolved inventory (finding 4); `w32_power_pids` already returns `-ENOSPC` on truncation | case 15: 64 targets, a 65th listed → `-ENOSPC` recorded; 32 exited → 32 new PIDs reclaim their entries; a program connecting with no entry reclaimable → overflow → busy dialog "Some programs could not be tracked" |

### Third-round results (host, systemd scope MemoryMax=3G)

- Launcher harness default run: shutdown sequence, G3 end-session and the
  18 T23 cases pass.
- `tests/host/power_runtime_test.c`: PASS (fence scenarios);
  `power_platform_test` 76 checks, `apm_platform_test` 103, `apm_t23_test`
  543, `power_sdk_test` PASS (they include `power_source.c`).
- `tests.host.win32.user32.test_user32` + `tests.host.win32.dialogs.test_dialogs`:
  23 tests OK (108 s; user32 now sends `DESK_V3_MODAL`).
- `test_memory_footprint` 2 OK; `test_window_model` 2 OK (11564 frames).
- Compile-only checks: `power.c`, `power_source.c` with the kernel's i686
  flags; `client.c`, `shell.c`, `desktop.c`, `win32_native.c`, `input.c`
  with `ciuki-cc -Werror`.
- Full desktop suite (`python3 -m unittest discover -s tests/host/desktop
  -t .`, 1855 s): 224 tests OK, 5 skipped.

### Third-round risks

- Programs now have 12 of the 16 slots (Task Manager and a prompt take a
  program slot while one is free, so the reserve costs nothing until the
  pressure point).
- The forced Off needs the platform's quiesce step (installed on every
  boot except safe mode and hardware check); without it, or when a driver
  does not stop within 2 s, the user is told to use the power button. The
  forced restart never waits on it.
- `power_prepare_files` on the worker: a regular file the input thread
  opens after the snapshot (a settings save during the sequence) may be
  closed under it; the sequence is modal and such saves are retried.
- A modal prompt whose owner is not on the wire (owner closing or
  unregistered) is sent without `DESK_V3_MODAL` (the protocol requires an
  owner) and is not recognised as a prompt: the 20 s bound applies and the
  dialog offers Cancel.
- A thread inside a plain `fs_lock_take` still waits for its holder; the
  shutdown no longer waits for it (finding 1) and the forced action ends the
  machine with its resources retained.

## Host tests

- `tests/host/desktop/hang_close_test.h` (run by
  `tests.host.desktop.test_desktop.DesktopTests.test_shell_launcher`), five
  cases: full client table (End Task dialog and Shut Down open, About
  refused); End Task on a program that answers and declines (dialog at 5 s,
  SIGKILL); Alt+F4 twice (dialog at once; a prompting program asked once);
  Restart with one windowless Win32 process holding its query (dialog at
  5 s, End Task, restart completes); power sequence with a Win32 process
  that has no desktop client (SIGTERM at the deadline, named in the busy
  list, Force SIGKILL). Built against the previous `client.c`, `shell.c`,
  `input.c` and `win32_native.c`, each of the five cases fails.
- `tests/host/desktop/end_session_test.h`: the G3 Win32 case now models the
  program's save prompt as a window of its process (it had no window, which
  the new windowless rule would end after 5 s).
- `tests/host/desktop/shutdown_sequence_test.h`: Force is SIGKILL.
- `tests/host/desktop/memory_footprint_test.h`: programs fill the table up to
  the reserved slots.
- `tests/host/win32/user32/user32_test.c` `test_desktop_close`: SC_CLOSE
  delivery, SC_CLOSE ignored, Close grayed; fails on the previous user32
  (`USER CHECK ... error 1400`).
- Guest compile check: `client.c`, `shell.c`, `input.c`, `win32_native.c`
  with `ciuki-cc -Werror` (desktop and control flavours); user32
  `desktop.c`, `messages.c` with the PE32 flags.
- Runs (systemd scope, MemoryMax=3G), first round: `python3 -m unittest
  discover -s tests/host/desktop -t .` 224 tests OK (5 skipped, 1862 s);
  `tests.host.win32.dialogs.test_dialogs` and
  `tests.host.win32.user32.test_user32` 23 tests OK. Second round: see
  "Second-round results" above.

## What the lead must check (QEMU, then T23)

1. Start the Quake III Arena demo installer four times in a row with Files,
   Notepad and NetSurf open. Expected: refused windows are recorded as
   `[desktop] client table full kind=... refused=...`; Ctrl+Alt+Del (Task
   Manager) and Shut Down still open.
2. Click the close button of an installer background: nothing happens (as
   on Windows 98; the caption returns from "Closing..."). Click it on a
   busy installer, then again: "This program is not responding" at the
   second click; End Task ends it (`[desktop] end task pid=N result=0`).
3. Task Manager End Task on a busy installer: the dialog after 5 s
   (`[desktop] end task timeout pid=...`), End Task ends it.
4. Restart with installers running: one `[desktop] end-session win32 turn`
   record per window asked; a busy one gets the dialog after 5 s; after End
   Task the session goes on; `[desktop] end-session agreed ... win32=committed`,
   then `[desktop] power win32 end-session ended after N ms` before any
   SIGTERM to a Win32 program; the machine restarts. If a program survives,
   "Some programs are still running" names it, Force ends it (SIGKILL) and
   the restart completes. No forced power-off may be needed.
5. Check that `[desktop] end task pid=... result=0` is followed by the
   process leaving the `[cpu] task=` lists: a Win32 thread blocked in a
   storage wait must end on SIGKILL (the request path is cancelled:
   `cancelled_wait_test`; a thread inside `fs_lock_take` waits for its
   holder, which the 5 s shutdown bound no longer depends on).
6. Second round, QEMU first: with an installer copying files, Shut Down
   while a 48 MB read holds C: for longer than 5 s. Expected:
   `[storage] shutdown busy holder=reservation wait_ms=5000 result=-16`
   (or `holder=writer`), the desktop's progress window keeps painting, the
   busy dialog after the EBUSY retries, Force (SIGKILL), and if the kernel
   still answers EBUSY a second Force with
   `[power] forced reboot requested ... [power] forced flush result=...`
   and a reset; at the next start ScanDisk checks C: if the flush did not
   complete. Also Ctrl+Alt+Del with 13 program windows open: Task Manager
   must appear (`[desktop] emergency connection admitted`).
7. Second round, broker: kill w32srv (or hold it busy) and Shut Down.
   Expected: `[desktop] end-session win32 broker begin failed ... retrying`,
   after 5 s `... programs not asked`, the busy dialog with "could not be
   asked to save their work", Force, shutdown completes.
8. Third round: End Task on an installer stuck in a disk wait during
   Restart. Expected: `[desktop] end task pid=N result=0`, no second dialog,
   after 5 s `[desktop] end-session win32 pid=N survived End Task ... programs not asked`,
   the busy dialog naming it, Force, "Force again", then
   `[power] forced reboot requested`, `[power] forced flush result=...`,
   `[power] forced device fence result=...` and the reset. For Shut Down
   the same, and with a failed fence the message "turn the computer off
   with its power button" instead of soft-off.
9. Third round: while the progress window shows "Shutting down..." after
   the programs closed, the Cancel button must disappear once call 73 runs
   (and Esc does nothing); with 12 program windows open, Ctrl+Alt+Del, a
   program's "Save changes?" prompt, End Task and Shut Down must all open
   together.

## Open points (not changed here)

- The `donation_cycle` records pair installer threads with desktop worker
  threads on drive and cache holds; if such a cycle is a real deadlock only
  a request deadline breaks it. The shutdown no longer depends on it (5 s
  bound, forced actions), and a request-scoped wait ends on SIGKILL
  (`cancelled_wait_test`), but a thread inside a plain `fs_lock_take` (no
  request scope) still waits for its holder; storage and scheduler owners
  should check it with these records.
- The Windows 98 timeout figures (5 s, 20 s) are the documented Windows
  2000 defaults, not Windows 98 measurements (see the second round).
- `device-work#9` took 17–34 % of the CPU in kernel mode with no DOS VM
  running; every program was slower for it.
- Every USER pump asks w32srv for session messages every 250 ms with a
  synchronous RPC; a w32srv blocked on a C: commit delays every Win32 UI
  thread. A notification instead of a poll would remove that dependency.
- The table itself stays at 16 slots: every Win32 top-level window, hidden
  ones included, takes one, so a few installers still exhaust it (their
  pages then fail and VISE goes on as if Next had been pressed). A larger
  table costs about 35.6 KB of desktop memory per slot (`sizeof(struct
  desk_client)` on the host build) and must change Task Manager's
  `TM_WINDOWS` and `SYSREQ_CLIENTS` with it; hidden and message-only Win32
  windows could also stay off the desktop.
- Task Manager is a program loaded from C:; under heavy disk load Ctrl+Alt+Del
  may take seconds, where Windows 98's Close Program is resident.
- The image records neither the first window of a Win32 program started
  through `shell-launch` nor the exit of a Win32 process; both would have
  dated this report.
