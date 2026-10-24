# G30 system tools: System Information, System Monitor, Resource Meter, device actions, Add New Hardware, Scheduler

Written 2026-10-23 (G30, directive `g17-g32-os-gaps-batches-4-5.md`; inventory rows SY-1, SY-2, SY-3,
SY-7, CP-2, CP-19). This delivers the pure models and host tests (`apps/systools`); windows, tray
wiring and kernel/loader consumption are listed as gaps at the end.

## Windows 98 reference (Resource Kit text in `build/rskt`, read-only)

- Ch. 27 "Troubleshooting Tools" (`wrkc27_2.htm`): MSInfo has three categories, Hardware Resources,
  Components and Software Environment, plus a System Information summary and a Tools menu; the Scheduled
  Task Wizard runs ScanDisk, Disk Defragmenter and Backup at regular intervals; the Maintenance Wizard
  schedules ScanDisk, Disk Cleanup, Disk Defragmenter and other tools.
- Ch. 26 "Tracking Performance with System Monitor" (`wrkc26_9.htm`): charts are edited with Add Item,
  Remove Item, Edit Item with an Explain definition; views are Line, Bar and Numeric charts; logging is
  started and stopped from the File menu.
- Ch. 24 "Configuring Device Settings" (`wrkc24_3.htm`): a conflicting Plug and Play device is disabled to
  free its resources; Update Driver starts the Upgrade Device Driver Wizard; hardware profiles select the
  drivers to load; legacy devices are found by Setup or the Add New Hardware Wizard.
- App. D (`wrkappd_3.htm`): "System Monitor" and "System Resource Meter" are optional components.

## Decisions

1. **No ABI change.** The models consume `struct ciuki_devices_info` (call 76), call 70 system records
   and plain input structs; no syscall or message number is added. Persistence is a text file the caller
   reads and writes with the native VFS (`dc_save/dc_load`, `st_save/st_load`). `abi_layout_test.c` is
   untouched.
2. **System Information** (`si_build`): categories as MSInfo. Conflicts/Sharing reports I/O or memory
   overlap and identical DMA as conflicts and a shared IRQ as sharing (PCI interrupt sharing is legal);
   unknown-size BARs show their base only, following `device-manager.md`. Problem Devices lists
   quarantined devices with their retained reason.
3. **System Monitor** (`sm_*`): rates need two snapshots, so the first only sets the baseline; counter
   wrap or a clock stepping back yields 0, never a huge rate. Auto scale uses the running peak. A counter
   the source cannot provide is refused (`-ENODEV`): the kernel exposes no disk byte counters yet, so the
   file-system counters are defined but unavailable. A full log stops logging; charts continue. Default
   interval 5 s as in Windows 98; accepted range 1..300 s.
4. **Resource Meter** (`rm_*`): CiukiOS has no 16-bit heaps, so User and GDI are the occupancy of the
   desktop's window-handle and GDI-object tables; System is the lower of the two, as in Windows 98.
   Levels: low below 30 %, critical below 15 % (CiukiOS choice; the values are constants).
5. **Device actions** (`dc_*`): Disable and Remove are refused for system devices and disks (the boot
   disk cannot be told apart from others at this layer). Disable releases the device's resources from the
   conflict computation (`dc_resource_count`) and needs a restart only when the device is working. Remove
   is undone by a restart (Plug and Play re-detection) unless the device is disabled; a driver override
   survives. Entries equal to the default are dropped, bounded at 32. Single hardware profile
   ("Original Configuration"); multiple profiles remain a gap.
6. **Add New Hardware** (`aw_*`): the search path lists retained devices that are quarantined or still on
   firmware settings and not disabled; with none, the wizard offers the manual hardware-type list; the
   model list is the caller's catalog filtered by type; finishing records a driver override via
   `dc_apply` and asks for a restart. A type with no device waiting for a driver ends with `-ENODEV`.
7. **Scheduler** (`st_*`): proleptic Gregorian arithmetic, minutes since 1970-01-01; monthly tasks on day
   31 run on the last day of shorter months. A run missed while the computer was off runs once at the
   next check, never once per missed day (CiukiOS choice); a task never runs for occurrences before it
   was created. One-time tasks disable themselves after running. At-start tasks run when the scheduler
   starts.
8. **Maintenance Wizard** (`mw_apply`): slots Night 00:00, Day 12:00, Evening 20:00; Express schedules
   all three tools weekly on Sunday, staggered 30 minutes; Custom chooses the tools; running the wizard
   again replaces its own tasks. The commands name the G29 tools (`/bin/defrag`, `/bin/scandisk`,
   `/bin/cleanmgr`) which do not exist until G29 merges.

## Verification

`tests/host/desktop/test_systools.py` (ASan/UBSan) covers each model; wired into
`scripts/test/host_kernel_tests.sh`. Sources compile with `ciuki-cc -O2 -Wall -Wextra -Werror`.

## Gaps

Windows for System Information, System Monitor, Resource Meter tray icon, Scheduled Tasks folder, the Add
New Hardware wizard pages and the Device Manager buttons; the scheduler service; kernel and loader
consumption of the stored disable/remove/driver state (BT-10 skip list overlaps); disk and file-system
counters in the kernel; Hardware Profiles; the Performance tab; the G29 maintenance tools.

## Windows for System Information, System Monitor and Resource Meter (G30 UI part A)

Sources: `apps/systools/{sysinfo,sysmon,resmeter}_{main,view}.c`, helpers `sysui.h` (view side) and `sysreq.h`
(desktop requests, mains only). Each `*_view.c` talks to an injected host struct, so
`tests/host/desktop/systools_ui_test.c` runs the production views on the real toolkit.

- **System Information** (640x480, resizable): tree of the `si_tree` categories, list of the selected category's
  children (a sub-category row shows its summary, or "n item(s)"; Enter or double click goes into it). Windows 98
  MSInfo menus: File (Save As Text File = `si_export` through the save dialog, Exit), View (Refresh, F5), Tools
  (Hardware Report, System Monitor, Resource Meter, Task Manager, Control Panel: `DESK_LAUNCH` by catalogue name
  without argument; a refusal, immediate or in the `TASK_RESULT`, is a message box). Conflicts carry the warning
  icon, shared IRQs the information icon, problem devices the error icon. Refresh keeps the selection and the
  expanded categories (by label path). RAM from `ciuki_hardware_inventory` (usable bytes), free pages and process
  count from call 70, User resources from a `DESK_WINDOWS_QUERY` window count over 16 client slots (until the
  desktop answers, User takes the memory share).
- **System Monitor** (560x420, resizable, Resource Kit ch. 26): one chart area drawn by the application (all items
  overlaid in their colours, which `DU_GRAPH`, a single green series, cannot do) as Line, Bar or Numeric chart
  (View menu, check-marked, Ctrl+L/B/N), a legend list (colour swatch, value, maximum) whose selection drives
  Edit > Remove/Edit/Explain, a status bar. Add Item and Edit Item are toolkit property sheets: Add lists the 11
  counters with category, marks counters the source lacks "(not available)" and refuses them with a message (via
  `sm_add`'s `-ENODEV`; nothing is faked), its Explain button shows `sm_explain`; Edit Item picks one of the 8
  palette colours and Automatic or a fixed maximum. Options > Chart sets the update interval (1..300 s; an invalid
  value is reported and asked again). A timer calls `smv_sample`: call 70 plus the network frame counters feed
  `sm_raw_from_proc`/`sm_sample`, and only the chart and legend are repainted. File > Start Logging opens the
  chosen file (truncated), writes the `sm_log_header` line and one `sm_sample` CSV row per step; a full row buffer
  or a write error stops logging and says so, the charts continue. Adding or removing items while logging is
  refused (the CSV columns would change). Disk counters come from the call-70 KiB totals, network errors stay
  unavailable.
- **Resource Meter** (320x180, fixed): System, User and GDI bars coloured green, amber or red by `RM_LOW_PCT` /
  `RM_CRITICAL_PCT`, percentages, one summary line, Close, refreshed every 5 s. User is the free share of the
  desktop client table (window count of a `DESK_WINDOWS_QUERY`, cap 16), **GDI is the free share of physical pages**
  (surfaces are page-backed; CiukiOS has no GDI object table), System the lower. When the desktop does not
  answer, User shows "n/a" and System is the GDI share alone. The desktop tray icon is the lead's part.
- Verification: `tests/host/desktop/test_systools_ui.py` (ASan/UBSan, pinned render digests, review PNGs via
  `CIUKI_FONT_REVIEW`). Not covered on the host: the mains (channel, syscalls), which compile with `ciuki-cc`.

## Scheduled Tasks program (G30 UI part B: `schedtask`)

Files: `apps/systools/schedtask_main.c` (window, file, clock, `/Programs` listing, launcher),
`schedtask_ui.c/.h` (folder, wizard, properties, Maintenance Wizard; no syscalls, testable on the host through
`struct stui_host`), tests `tests/host/desktop/schedtask_test.c` and `test_schedtask.py` (pinned renders).

### Windows 98 behaviour relied on (Resource Kit `wrkc23_4.htm`, `wrkc27_2.htm`, `wrkc10_4.htm`)

- Scheduled Tasks is a folder opened from My Computer; **Add Scheduled Task** is its first item and starts the
  Scheduled Task Wizard; double-clicking a task opens its properties with three tabs: **Task** (the program),
  **Schedule** (frequency, date, time) and **Settings** (when to delete or stop the program, idle and power
  conditions). Tasks are `.job` files in `\Windows\Tasks`.
- Maintenance Wizard (Tuneup.exe): schedules ScanDisk, Disk Cleanup, Disk Defragmenter (and Compression Agent); the
  user picks the time of day or night, or customizes the schedule; its tasks appear in the Scheduled Tasks window
  with the prefix "Tune-up" or "Maintenance"; `Tuneup /autorun` runs the tasks in succession without changing the
  schedule. The Resource Kit has no text for the wizard pages, so the page wording is CiukiOS's own.

### Behaviour implemented

- Window "Scheduled Tasks" 640x380: details list (Name, Schedule, Next Run Time, Last Run Time, Status) whose first
  row is Add Scheduled Task; menus File (Open, Run, Delete, Properties, Exit), Advanced (Maintenance Wizard,
  View Log), Help (About); right-click or Menu key gives Run, Delete, Properties; Delete key asks first; F5 and a
  15 s timer re-read the file. Times are local time ("2026-10-24 09:00", 24-hour). Next Run Time is "Due now" when
  an occurrence is overdue (the service runs it at its next check), "Never" for disabled and finished tasks,
  "At system startup" for those. Status: Ready, Disabled, "Last run failed (error N)"; "Running" is not tracked
  (the scheduler records the launch, not the end of the program).
- Run: `ss_split` the command and send `DESK_LAUNCH` by catalogue name (`sysreq_launch`); the `TASK_RESULT` reply
  records last run and result in the file; failures (refused request, -ENOENT, ...) show a message with the error
  number. The desktop launcher starts programs by name only: arguments of the command are not passed (see
  the gaps below).
- Add Scheduled Task wizard (pages built into the window: a toolkit dialog holds only six caller controls and the
  wizard needs up to twelve): Welcome; Program (the sorted `/Programs` list with catalogue labels and icons, plus a
  text box for an absolute path with optional arguments, which wins over the list); Name and frequency (Daily,
  Weekly, Monthly, One time only, When my computer starts); Time and date (HH:MM 24-hour, YYYY-MM-DD, weekday
  check boxes, day of month; skipped for "When my computer starts"); Finish (summary and "Open advanced properties").
  Messages: bad time/date, no weekday, day not 1..31, one-time start already past, empty/long/`=` names, duplicate
  name (-EEXIST, also checked on the name page), full folder (-ST_MAX tasks, -ENOSPC), unreadable or unwritable file.
- Properties: Task (Run command line, Enabled), Schedule (kind, time, start date, weekdays, day of month, next run),
  Settings (last run, last result, status). OK / Apply / Cancel. A changed schedule sets `created` to now so an
  edited never-run task does not run for earlier occurrences; `last_run` is kept, so editing the time of a task that
  has run before can make the model's "run a missed occurrence once" rule fire once at the next check.
- Maintenance Wizard (Advanced menu): Express (recommended: all three tools) or Custom (tool check boxes); slots
  Nights 00:00, Days 12:00, Evenings 20:00; result message with the number of tasks; running it again replaces its
  own tasks (`mw_apply`). The tools are the G29 disk tools: until G29 merges the tasks are created but their
  programs may not exist, and the message and welcome page say so.
- Persistence: every edit re-reads `SS_PATH` first and writes the whole file back (temp file next to it, fsync,
  rename, EEXIST fallback unlink+rename), so a run the scheduler service recorded in between is kept; a real read
  error (not -ENOENT) cancels the edit and the file is not rewritten. Lines `st_load` cannot parse are dropped by the
  rewrite. Time source: libc `localtime_r` (system zone); `--test=schedtask` fixes it at 2026-10-11 09:41.

### Decisions needed / gaps

- "Delete the task when it has run" (Settings tab) needs a field in `struct st_task` and the file format; the model
  disables one-time tasks after running, which the Settings tab states. Not changed (models are the lead's).
- Task names created by the Maintenance Wizard have no "Maintenance"/"Tune-up" prefix (names come from `mw_apply`);
  `Tuneup /autorun` (run all in succession) is not implemented.
- Arguments are not passed on a manual Run (`DESK_LAUNCH` carries a name only). Existence of a typed path or of the
  G29 tools is not checked. Settings conditions (idle, power), Advanced > Stop/Pause Task Scheduler, Task Scheduler
  tray icon and drag and drop of programs into the folder are not implemented. No file lock: a write racing with
  the service between re-read and rename can still lose that one update.

## Control Panel part: device actions, Add New Hardware, Performance, Hardware Profiles

Written 2026-10-24 (G30, Control Panel). Code: `apps/control/hardware.inc` (included by `app.c`), tests
`tests/host/desktop/control_system_tools_test.c` and `test_control_system_tools.py`.

Windows 98 reference (Resource Kit ch. 24, `wrkc24_3.htm`): a device's Properties > General has "Disable in this
hardware profile"; the Device Manager has Remove and Refresh; Update Driver starts the Upgrade Device Driver Wizard;
hardware profiles select which drivers load; legacy devices are found by the Add New Hardware Wizard; the System
Properties sheet has General, Device Manager, Hardware Profiles and Performance tabs, the Performance tab reporting
memory, system resources, file system, virtual memory, disk compression, PC Cards and a verdict.

Decisions:

1. **Device Manager buttons** sit on the Device Manager page, under the tree: Disable, Enable, Remove, Update
   Driver..., Refresh (Refresh already existed). A message dialog holds at most four buttons and the Properties dialog's
   General/Resources/Close indexes are part of the existing tests, so the Properties dialog keeps its shape and reports
   the state instead: "This device is disabled.", "(removed, detected again after a restart)", the pending driver,
   the hardware profile, and no resource list while the device is disabled or removed (`dc_resource_count`).
2. **Actions** go through `hp_apply` on the active profile. Disable and Remove ask first ("Do you really want to
   disable it?", Windows 98 wording); critical devices (`dc_critical`) and absent devices skip the question and show the
   model's refusal. The result dialog shows the model message and "Restart the computer for the change to take effect"
   when `needs_restart`, offering "Restart..." (the shell's safe shutdown dialog) or "Later".
3. **State file** `/system/settings/hwprofiles.cfg` (`hp_save`/`hp_load`): loaded at start, saved after every successful
   action with temp file, fsync and rename (`cfg_write_at`). A failed save restores the previous in-memory state and says
   so. An unreadable file keeps the defaults and says so; an absent file is normal. Host seams: `hw_load`, `hw_save`.
4. **Update Driver** lists the drivers of the selected device's class from the catalogue, or, when the catalogue has
   none, asks for a driver name that `dc_apply` validates. The catalogue (shared with Add New Hardware) is: built-in
   drivers (`drivers[]` of call 76) once per class of the devices bound to them (a driver bound to no device has no
   known class and is not listed, so nothing is invented), plus installed driver-pack manifests for the NETWORK class
   (`dp_catalog` over `/System/drivers`, seam `driver_catalog`; the package service is the driver name).
5. **Add New Hardware** is the applet "Add New Hardware" (Power and devices), the `aw_*` pages: Welcome, search yes/no,
   searching (a progress page of five 150 ms steps before the synchronous scan, cancellable), found list or the
   none-found page, hardware types, models, Finish, Done with the restart note. The scan re-reads the inventory.
   Finish with no device waiting for a driver reports that and records nothing.
6. **Performance** tab: `pf_summarize`. Memory and free percentage come from call 70 (seam `memory`, current values),
   the percentage is free physical pages (not the User/GDI heaps). PC Card sockets are counted CardBus bridges (PCI
   class 6.7) in the inventory; acceleration comes from the accelerator device record or the engine query. There is
   no user-mode query for the pager, so Virtual Memory reads "Managed automatically by the system" (the model's
   swap-file line and its "virtual memory is off" verdict are bypassed). File System, Graphics and Virtual Memory
   buttons are shown disabled with the label that they are managed automatically in this release.
7. **Hardware Profiles** tab: list with Current/Default/Portable, Properties (name, portable computer), Copy, Rename,
   Delete (confirmation; the model's in-use and last-profile refusals are shown), Move Up/Down, Set Default, and the
   start-up choice (wait for me, or the default after N seconds, 0..999). Only the active profile receives device actions.
8. `hp_save` held a 2048-byte scratch for one profile; 32 device settings need up to 2411 bytes. It is 2560 now
   (defect found by the Control Panel test, fixed in `apps/systools/profiles.c`).

Gaps: the start-up profile menu needs a loader/kernel hook that does not exist, so profiles are only edited and
stored; `hp_store.active` is whatever the file says (first profile by default) and nothing changes it; the kernel does
not read the stored disable/remove/driver state yet; the General tab of System Properties still reports memory read at
start-up; the pager has no user-mode query.
