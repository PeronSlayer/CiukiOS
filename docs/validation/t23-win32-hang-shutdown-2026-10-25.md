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
| Programs leave `DESK_SYSTEM_SLOTS` (2) client slots for End Task, Shut Down, the shutdown progress and messages; other built-in windows leave one; refusals recorded | `apps/desktop/desktop.h`, `apps/desktop/client.c` (`desk_add_client`, v3 CREATE_WINDOW, `desk_free_slots`), `apps/desktop/shell.c` (`desk_internal_open`) |
| Win32 turn: a process that holds its query for 5 s with no window on the desktop is not responding; every window in turn is recorded (`[desktop] end-session win32 turn pid=... hwnd=... delivered=...`); a failed End Task dialog is recorded | `apps/desktop/client.c` (`session_hung`, `session_step`, `desk_end_task_open`) |
| Power sequence: every live Win32 process from the broker (`W32R_SHUTDOWN_LIST`) is a target (closed, SIGTERM at the 3 s deadline, listed as "Windows program (PID n)"); Force sends SIGKILL | `apps/desktop/client.c` (`desk_power_begin`, `power_terminate`, `desk_power_force`), `apps/desktop/win32_native.c` (`w32_power_pids`), `apps/desktop/desktop.h` (`power_w32_pids`) |

Design record: `docs/design/desktop-shell.md` (G3, "Amendments after the T23
trial of image c42f556e") and `docs/design/win32-personality-contract.md`
("t23close").

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
- Runs (systemd scope, MemoryMax=3G): `python3 -m unittest discover -s
  tests/host/desktop -t .` 224 tests OK (5 skipped, 1862 s);
  `tests.host.win32.dialogs.test_dialogs` and
  `tests.host.win32.user32.test_user32` 23 tests OK.

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
   Task the session goes on; the machine restarts. If a program survives,
   "Some programs are still running" names it, Force ends it (SIGKILL) and
   the restart completes. No forced power-off may be needed.
5. Check that `[desktop] end task pid=... result=0` is followed by the
   process leaving the `[cpu] task=` lists: a Win32 thread blocked in a
   storage wait must still end on SIGKILL (kernel side, not changed here).

## Open points (not changed here)

- The `donation_cycle` records pair installer threads with desktop worker
  threads on drive and cache holds; if such a cycle is a real deadlock only
  a request deadline breaks it, and SIGKILL must still end a thread waiting
  there. Storage and scheduler owners should check it with these records.
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
