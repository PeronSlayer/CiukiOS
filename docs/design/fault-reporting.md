# Fault reporting, shell restart, fatal screen and log viewer (G13)

- **Written:** 2026-10-23 23:30 CEST for directive G13 (`docs/directives/g13-g16-os-gaps-batch-3.md`).
- **Rows:** ER-1, ER-2, ER-3, ER-5, SY-8 of `docs/design/os-feature-inventory.md`.
- **Evidence:** host tests only (no QEMU, no image); see "Tests".

## Research and reference behaviour

| Source | What it fixes |
| --- | --- |
| Windows 98 Resource Kit, Ch. 27 Table 27.1 and "Using Dr. Watson" (`build/rskt/wrkc27_2.htm`) | Dr. Watson traps application faults, logs the faulting program, the module and the address to a file, and cannot snapshot a system hang. CiukiOS writes the same facts to a crash file |
| Resource Kit Ch. 17 (`wrkc17_17.htm`) | The user-visible wording: "This program has performed an illegal operation and will be shut down." |
| Resource Kit Ch. 10 (`wrkc10_9.htm`) | The blue screen: "A fatal exception 0E has occurred at 0028:C3C64C51 in VXD IOMEGA (01) + 00000CB5. The current application will be terminated." |
| Resource Kit Ch. 28 "User Interface" (`wrkc28_9.htm`) | The shell is an ordinary Win32 process at the level of other applications; it can fail and restart on its own |
| Microsoft Learn, [The Taskbar, "Taskbar Creation Notification"](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar) | A restarted shell is a new taskbar; programs that had tray icons add them again. CiukiOS programs reach the desktop through a channel that ends with the desktop process, so they end with it; a notification message is not modelled |
| Intel SDM Vol. 3, "Interrupt and Exception Handling" (vector names, error codes) | Vector names, the page-fault error bits (P, W/R) and the selector error code of #GP, used by the fatal screen and the Details text |
| PS/2 scan code sets (OSDev wiki "PS/2 Keyboard", not fetchable from this host (HTTP 403); checked against `src/kernel/drivers/i8042.c` `set1[]`/`set1_e0[]`, which cite Linux `atkbd_unxlate_table`) | Set 2: Ctrl 14, Alt 11, Enter 5A, Esc 76, E0 71 Delete, F0 = break prefix; set 1: 1D, 38, 1C, 01, E0 53 |

## ER-1, SY-8: a native program is shut down by a fault

1. `proc_signal_fault` (`src/kernel/proc/signal.c`) is where a ring-3 fault of a native process kills it (no handler, the signal blocked or ignored, or already in a handler). Before `proc_signal_die` it calls `native_fault_report`. A program that catches SIGSEGV and survives is not reported; a signal sent with `kill` is not a fault. A fault whose handler frame cannot be built (bad stack) is also not reported (the user sees the program vanish; rare, listed under gaps).
2. `native_fault_report` (`src/kernel/proc/win32/diagnostics.c`) writes a `[fault] native pid= tid= name= vector= error= eip= address=` line to the boot log, and, only when a desktop is running or being restarted, queues a `struct ciuki_w32_fault` of kind `W32_REPORT_NATIVE_FAULT` (new constant 6, no new syscall and no struct change: the 224-byte record is reused). Field use: `code = vector | error << 8`, `offset` = faulting address (CR2 for #PF, else EIP), `registers` = eax ebx ecx edx esi edi ebp esp eflags, `stack` = eight words at ESP with a valid mask, `exe` = program name, one record per process. The queue is the existing 16-entry Win32 desktop mailbox, so the dialog and the file writer need no new transport.
3. The desktop (`apps/desktop/win32_native.c` `fault_poll`) takes the record, starts the crash-file worker thread and opens the dialog (`desk_w32_fault_dialog`): title = program name, text "This program has performed an illegal operation and will be shut down.", "If the problem persists, contact the program vendor.", Close and Details (Tab or the button; Details shows `desk_native_fault_text`: program, PID, thread, a plain-words line such as "Page fault: write of unmapped address 00000010", vector and error code, EIP, the registers and the stack words). The program is already gone when the dialog appears (Windows 98 shuts it down on Close; the effect for the user is the same). The system keeps running.
4. The file: `/system/logs/CRASH0.TXT` to `CRASH7.TXT`, written by `desk_w32_crash_store` with the same rotation as the Win32 crash files (the slot with the lowest sequence number is replaced; first line `CiukiOS crash sequence=N`), at most 2 KiB each, text only, no memory dump (the Win32 rule). The path is the C:\SYSTEM\LOGS of the directive.

## ER-3: the desktop is restarted

`supervisor_poll` (`src/kernel/proc/supervisor.c`) now treats the death of the bootstrap desktop as follows.

- Clean exit (status 0), a probe desktop, the stand-in, or more than three restarts in 60 seconds: as before, the console takes over (`desktop_gone`).
- Any other end (a fault, a signal, a non-zero status, or an unknown end): `desktop_restarting` is set, `desktop_deaths` counts the death, the supervisor loop is woken again, and each pass waits (1 ms sleeps, at most 5 s) until the old desktop is a zombie or gone, which means `close_resources` released its display and input grants (`grants_install` refuses while the previous owner holds them), then spawns `/Programs/desktop` again exactly as the bootstrap does (`supervisor_spawn`, grants, mixer and task authority). If that fails, the console takes over and the failure is logged.
- The fault record of the crashed desktop stays in the kernel mailbox, so the new desktop shows "desktop has performed an illegal operation" and writes the crash file for it.
- Limits: the windows of other programs belong to the dead desktop's channels, so those programs lose their window (what they do then is up to them; not exercised here), and the Win32 window server `w32srv` was spawned by the old desktop (`win32/service.c` `w32_service_delegate`), so running Win32 programs are expected to be lost too. `supervisor_desktop_pid()` is 0 during the restart, so Win32 services and display authority refuse until the new desktop is up.

## ER-2: the fatal-error screen

`panic`, `panic_frame` and the double-fault handler (`src/kernel/core/output.c`) keep their serial and black-box records and the `PANIC` record as the last serial output, then call `fatal_screen` (`src/kernel/core/fatal.c`), which draws on the console only (white on blue, the history scrolled away, one repaint):

```
  CiukiOS

  A fatal exception 0E has occurred at 0008:C0011E36 in task "desktop".
  page fault: write of unmapped address 00000010 (error code 00000002).
  The system cannot continue.

  *  Press ENTER or CTRL+ALT+DEL to restart your computer. You will
     lose any unsaved information in all programs.
  *  Press ESC to keep this screen and stop here.
```

A `panic()` without a frame prints "A fatal error has occurred in task ...:" and the reason with its spaces. The wait loop polls the 8042 (interrupts stay off): it reads the command byte once (bounded) to learn whether the controller translates to set 1, decodes Enter or Ctrl+Alt+Del as restart (`kernel_reboot`, which already has the 8042 pulse and triple-fault fallbacks) and Esc as stay. With no 8042 (`0xFF` status) or a USB-only keyboard the screen stays, as the old halt did. Windows 98's "press any key to continue" is not offered: a kernel fault has nothing to continue.

## ER-5: Log Viewer

`/Programs/logview` (accessory, Start > Programs > System Tools, Tango `text-x-generic`): a list on the left (Boot log, NetSurf log, Setup log when present, then "Crash N: program" for native files and "Win32 crash N: program" for `/WINDOWS/CRASH`, newest first) and a read-only text pane. Logs larger than the text area show their last 60 KB, starting on a line, with CR LF folded; the status bar says "last N of M bytes". A missing log says which component writes it. F5 or File > Refresh re-scans and keeps the selected file. Edit > Copy and Select All work on the pane.

## Tests

| Behaviour | Test |
| --- | --- |
| Fatal text, clipping, truncation, set 1 and set 2 restart choice, Ctrl+Alt+Del, Esc | `tests/host/fatal_test.c` |
| Native fault record (registers, stack, address, packing, one per process, none without a desktop, queue full, Win32 and kernel-mode frames ignored) | `tests/host/win32/diagnostics/kernel_test.c` `native_faults` |
| The signal path reports exactly the fatal faults | `tests/host/proc/signal_test.c` `test_fatal` |
| Restart: grants wait, three per minute, window expiry, clean exit, probe, failed start | `tests/host/proc/desktop_test.c` `test_desktop_restart` |
| Crash file rotation and content for native records | `tests/host/win32/diagnostics/crash_file_test.c`, `test_diagnostics.py` |
| Dialog wording, Details, Close, goldens `native-fault`, `native-fault-details` | `tests/host/desktop/v3_test.c`, `v3-goldens.json` |
| fx-15 row 31 (dialog frames equal a full redraw on five presenters) | `tests/host/desktop/window_model_test.c` `row_fault` |
| Log list, tail, window, refresh, renders | `tests/host/desktop/logview_test.c` via `test_accessories.py` |

## Remaining gaps

- A fault whose signal handler frame cannot be built is not reported.
- DOS VM faults and Win32 faults keep their own paths (W32-8 and the VM monitor).
- The Control Panel Logs tab is unchanged; it does not open the Log Viewer.
- Programs do not get a `TaskbarCreated`-style message after a shell restart.
- The QEMU crash-isolation probe kills victims with faults: those now queue unread native records (bounded at 16) on the probe desktop. Harmless, not exercised (QEMU is suspended).
