# sec-sysvc2: one log location, SYSTEM jobs, Registry Checker in the broker, sealed boot settings

Directive: `docs/directives/sec-sysvc2.md` (2026-10-28). Base: `wt/sec-integ4` with
`wt/sec-sysvc` merged. Contracts: `docs/design/user-security-kernel.md` (rule table,
"SYSTEM services for administrator operations and logs" and its sec-sysvc2 amendment
"SYSTEM jobs, sealed boot settings and Registry Checker"),
`docs/design/user-security-contract.md` 8.2 A1-10 (sec-sysvc2 amendment),
`docs/design/startup-recovery.md` 2.5 (sec-sysvc2 amendment). Earlier records:
`docs/validation/sec-sysvc.md`, `docs/validation/sec-user-05.md`.

## Research (Windows XP, Windows 98)

Each fact was read on the page cited. Pages that are not Microsoft's are marked
"secondary".

1. **Profile layout.** `CSIDL_APPDATA` is "the file system directory that serves as a
   common repository for application-specific data", typically
   `C:\Documents and Settings\<user>\Application Data`; `CSIDL_LOCAL_APPDATA` is the
   "data repository for local (nonroaming) applications", typically
   `C:\Documents and Settings\<user>\Local Settings\Application Data`;
   `CSIDL_COMMON_APPDATA` (`All Users\Application Data`) holds data that is not
   user-specific, "for example ... a log file"; applications should not create files
   at the profile root
   ([CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl)). The XP
   profile article: `Local Settings` holds data that does "not roam with the profile.
   Usually either machine specific, or too large to roam effectively"; its
   `Application Data` is computer-specific application data, beside `History`,
   `Temp` and `Temporary Internet Files`; top-level `Application Data` is per-user
   roaming data; "Application vendors decide what data to store in this directory"
   ([User Data and Settings Management, Windows XP](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-xp/bb490855(v=technet.10))).
   The `<Company>\<Product>` subfolder convention is common practice; no Microsoft
   page stating it as a rule was found.
2. **Crash reports.** Dr. Watson's `Drwtsn32.log` goes to
   `HKLM\SOFTWARE\Microsoft\DrWatson\LogFilePath`, by default
   `All Users\Application Data\Microsoft\Dr Watson`; "All users must have permission to
   read and write to the log file"
   ([LogFilePath Entry](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc776594(v=ws.10))).
   Windows Error Reporting on XP/2003 queues reports per user in
   `%USERPROFILE%\Local Settings\Application Data\PCHealth\ErrorRep`, or machine-wide in
   `%SYSTEMROOT%\PCHEALTH\ErrorRep\QSignoff`
   ([StreamInsight: error reports](https://learn.microsoft.com/en-us/previous-versions/sql/streaminsight/ff518560(v=sql.105))).
   Kernel minidumps go to `%SystemRoot%\Minidump`
   ([Read small memory dump files, KB 315263](https://learn.microsoft.com/troubleshoot/windows-client/performance/read-small-memory-dump-file)).
   So XP keeps a user program's own crash reports below that user's
   `Local Settings\Application Data\<vendor component>`, and system ones with the system.
3. **Event logs.** Logs are configured under
   `HKLM\SYSTEM\CurrentControlSet\Services\Eventlog\{Application,Security,System}` and read
   through the Event Log service; the default security descriptors give SYSTEM and
   Administrators read/write/clear of the Application log
   ([Eventlog Key](https://learn.microsoft.com/en-us/windows/win32/eventlog/eventlog-key));
   the Security log needs `SE_SECURITY_NAME` and only LSA writes it
   ([Event Logging Security](https://learn.microsoft.com/en-us/windows/win32/eventlog/event-logging-security));
   "Prevent local guests group from accessing ... log" applies to Windows 2000/XP/2003
   ([Event Log policy settings](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc736845(v=ws.10))).
4. **Scheduled system maintenance as SYSTEM.** System Restore's automatic checkpoint
   interval `RPGlobalInterval` defaults to 86,400 s, one a day
   ([SystemRestoreConfig, minimum client Windows XP](https://learn.microsoft.com/en-us/windows/win32/sr/systemrestoreconfig));
   a restore point is "a full snapshot of the registry and other system databases", and
   callers must let LocalSystem call back into them
   ([SRSetRestorePoint](https://learn.microsoft.com/en-us/windows/win32/api/srrestoreptapi/nf-srrestoreptapi-srsetrestorepointa)).
   The restore points live in `System Volume Information`, accessible only to the SYSTEM
   account by default (secondary: [System Restore](https://en.wikipedia.org/wiki/System_Restore),
   [neosmart wiki](https://neosmart.net/wiki/how-to-gain-access-to-the-system-volume-information-folder/)).
   Task Scheduler 1.0 is the XP scheduler
   ([Task Scheduler versions](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2008-R2-and-2008/cc727168(v=ws.10))).
   `autochk` "runs when the computer is started and prior to Windows ... starting" when
   a volume is dirty ([autochk](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/autochk));
   on XP its report is moved into the Application log by Winlogon (event 1001) at the
   next start, not by the user who logs on (secondary:
   [Ask Leo](https://askleo.com/chkdsk-results-after-reboot),
   [winhelponline](https://www.winhelponline.com/blog/how-to-read-chkdsk-log-event-viewer/)).
5. **Registry backup and restore are administrator tools.** XP System Restore: "Log on
   to Windows as Administrator" to restore or undo a restore, and the restore restarts
   the computer
   ([Windows XP System Restore](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-xp/bb457025(v=technet.10))).
   Windows 98 Registry Checker runs at every start, "backs up the registry only once per
   day" into compressed CAB files in `\Windows\Sysbckup` (`MaxBackupCopies=5`, 0-99), and
   ScanReg (MS-DOS) restores outside Windows (Windows 98 Resource Kit, `wrkc27_2.htm`
   "Using Registry Checker", Tables 27.9-27.10; local extract `build/rskt`, the media in
   `third_party/mswin98rskt`).
6. **Hardware profiles and machine settings.** Hardware profiles are machine registry
   data (`HKLM\SYSTEM\CurrentControlSet\Control\IDConfigDB`, `...\Hardware Profiles`)
   managed in Control Panel > System > Hardware
   ([Hardware profiles](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc728360(v=ws.10))).
   That only administrators change them, the paging file and the display acceleration
   level was not found stated on a Microsoft page. CiukiOS's own rule table decides
   here: boot inputs are SYSTEM maintenance only (K-Q4).

## Decisions

1. **One per-user log location: `<profile>\Local Settings\Application Data\CiukiOS\Logs`.**
   The kernel LOG service's USER scope (sec-sysvc) is kept; sec-user-05's
   `Local Settings\Application Data\NetSurf` and `\Crash` log folders are not used.
   Why: XP keeps non-roaming per-user data in `Local Settings\Application Data` below a
   vendor folder (fact 1), and crash reports of a user's programs in that user's
   `Local Settings\Application Data\PCHealth\ErrorRep`, the system component's folder
   (fact 2); CiukiOS's log and crash writer is the system's (the desktop and the kernel
   service), so `CiukiOS\Logs` is its component folder. One folder lets the service
   resolve it from the committed profile descriptor (the caller never names a path), and
   one LIST shows the Log Viewer everything. NetSurf's roaming data (choices, bookmarks,
   history) stays in `Application Data\NetSurf` (sec-user-05), as XP roams application
   settings.
   - Writers: NetSurf (`sdk/netsurf/port/chrome.c`, `NETSURF.LOG` through LOG APPEND
     USER), the desktop's crash writer (`apps/desktop/win32_native.c`/`win32_crash.c`,
     `CRASHn.TXT` native, `WCRASHn.TXT` Win32 through LOG REPLACE USER: the Dr. Watson
     style 8-slot rotation). Readers: Log Viewer (`apps/accessories/logview_model.c`,
     USER and MACHINE scopes). Control Panel's Logs tab and Hardware Report read the
     system logs (BOOT*.LOG) only; Dr. Watson-style files of earlier releases in
     `C:\WINDOWS\CRASH` stay readable to administrators.
   - Without the service (ENOSYS older kernel, ENODEV no managed system volume) a profile
     session's programs write the same folder (`profile.h` `PROFILE_LOGS`, the service's
     names); only the profile-less bootstrap desktop writes G13's machine folders.
     With the service, a session without a committed profile (the interim administrator
     session) keeps its logs in `C:\SYSTEM\LOGS\Users\u<uid>` (sec-sysvc, unchanged).
2. **SYSTEM jobs.** The desktop of every session asks; the work runs as SYSTEM (fact 4):
   - ScanDisk at-start report: call 99 op 9 JOB `STARTUP_REPORT` (kernel,
     `src/kernel/fs/sysvc_jobs.c`). Any live session subject (administrator, standard
     user, Guest, interim) or SYSTEM; the kernel appends each at-start check once per
     start and drive from its own record (`storage_scandisk_report`), so no caller text
     reaches the system log. The text is `ciuki/scandisk_text.h`, shared with the tools
     (`apps/sysrecovery/scanreport.c`): byte-identical to the base formatter on 200,000
     random records, with both `vsnprintf` and the kernel's `kvsnprintf`.
   - Daily Registry Checker backup: the registry broker's job (`RB_CHECKER_DAILY`), any
     session; at most once a local day by the broker's own clock and zone (call 97), so a
     request carries no time and cannot rotate older backups out. It is a broker job, not
     a kernel one, because item 3 puts the registry's backup in the broker.
3. **Registry Checker in the SYSTEM registry broker** (`apps/registry/checker.c`, engine
   `apps/sysrecovery/regcheck.c`, private ops `RB_CHECKER_*` in
   `sdk/private/registry_protocol.h`).
   - A backup holds what the broker owns since sec-k06: the native System and Programs
     roots (`C:\SYSTEM\SECURITY\REGISTRY\MACHINE\*.reg`, read after libreg's recovery),
     the committed Win32 machine hive slot (of `SYSTEM.CRG.0/.1`, the valid one with the
     higher sequence, which is what its loader takes: CRG1 header, CRC-32s, COMMITTED
     mark), plus BOOT.CFG, HWPROF.CFG and the `.cfg` files of `C:\SYSTEM\SETTINGS` (same
     CRB1 format, at most 4 MiB as before). Per-user hives are the
     users' data and are not backed up (XP's restore points snapshot the machine
     registry; not changed further here).
   - LIST, BACKUP, RESTORE and FIX: administrators (the interim administrator session
     too) and SYSTEM; EPERM for standard users and Guest (fact 5). STATUS (what the
     broker did at this start, damaged roots now, a pending restore) and DAILY: every
     session.
   - RESTORE records `RESTORE.PND`; the broker applies it at its next start before any
     root or hive is loaded (`rb_checker_boot`), as XP's System Restore restores while
     restarting. Every entry (known path, parsing root, valid hive slot) is checked
     before anything is written; the other hive slot is removed so it cannot outrank
     the restored one; a damaged or missing
     backup drops the request (no restart loop), an I/O error keeps it. `SCANREG
     /RESTORE` therefore no longer needs Command prompt only. A damaged native root is
     restored at that start too (Windows 98 ScanRegW at every start) and the desktop
     shows it from STATUS; so is the Win32 machine hive when no slot is valid (its
     loader would otherwise refuse to start the broker), but only at the start: a
     running broker rewrites a slot from memory itself, so `SCANREG /FIX` repairs the
     native roots only. Legacy entries of pre-sec-k06 backups (`/System/Registry/*.reg`)
     are skipped.
   - `C:\SYSTEM\SYSBCKUP` is SECRET (SYSTEM-only, like System Volume Information): the
     backups are the restore's trust input. One trie rule: 30 of 32 nodes.
4. **Sealed boot settings.** `C:\SYSTEM\HWPROF.CFG` (loader and kernel), and in
   `C:\SYSTEM\SETTINGS` `performance.cfg`, `hwprofiles.cfg` (its fallback) and the
   display attempt files (`accel*.cfg`: the DM8 keyed markers, the shared `accel.cfg`,
   the retry request; plus every registered driver's declared attempt files, asked from
   the display registry `display_attempt_file`) are SEALED by name in `policy_child`, as
   the hive slots are: no trie node, every direct writer refused (administrators too),
   everyone reads. Control Panel's Display (acceleration retry), Performance and
   Hardware Profiles/Device Manager pages save through call 99 op 10 SETTING
   (administrators and SYSTEM), which validates exactly what the loader and kernel
   accept (printable HWPROF.CFG up to 24,575 bytes, a complete valid performance record,
   the DM8 request) and rewrites the file as SYSTEM, restoring the old text on a failed
   write (BOOT_CONFIG's method). The kernel writes the attempt markers itself; the
   registry broker restores these files from a backup as SYSTEM; Setup and the image
   builder write them on an unmanaged volume. Other administrator-written settings in
   `C:\SYSTEM\SETTINGS` stay PROGRAM.

## Inventory of the writers changed

| Writer | Before (base) | Now |
| --- | --- | --- |
| Desktop crash reports (`win32_native.c`) | conflict: service vs `Local Settings\Application Data\Crash` | service (USER scope); fallback the profile's `CiukiOS\Logs` (CRASH/WCRASH names), or the machine folders without a profile |
| NetSurf log (`chrome.c`) | service; fallback `Local Settings\Application Data\NetSurf\NETSURF.LOG` | service; fallback `...\CiukiOS\Logs\NETSURF.LOG`, folder made on the first fallback |
| Log Viewer (`logview_model.c`) | conflict: files vs service | service (USER, MACHINE), Setup log and `C:\WINDOWS\CRASH` as files |
| Desktop at-start ScanDisk report (`desktop/recovery.c`) | appended as the user (EPERM for a standard user) | call 99 JOB, SYSTEM; the old append only on ENOSYS/ENODEV |
| Desktop daily Registry Checker backup | files as the user, raw `/System/Registry` roots (absent since sec-k06) | broker DAILY, SYSTEM |
| Desktop damaged-root restore at start | as the user | broker at its start; desktop notice from STATUS |
| SCANREG (Terminal), System Configuration backups | files as the user | broker, administrators; restore at the next start |
| Control Panel performance (`main.c` perf_save) | `cfg_write_at` performance.cfg | SETTING PERFORMANCE |
| Control Panel hardware profiles / Device Manager (`main.c` hw_save) | `cfg_write_at` HWPROF.CFG | SETTING HWPROFILES |
| Control Panel display retry (`main.c` accel_retry) | `accel-retry.cfg` file | SETTING ACCEL_RETRY |
| Kernel display attempt markers (`drivers/video`) | SYSTEM | unchanged (now sealed for everyone else) |

## Evidence (host; QEMU and hardware are the lead's)

- **H16** (`tests.host.security.test_system_services`, production call-99 adapter and
  `fs/sysvc*.c` on the native syscall/fd/POSIX namespace/legacy VFS/bound policy/OWNER
  stack, real FAT): JOB by Guest appends both at-start reports in the tools' format as
  SYSTEM, every other session (standard user, administrator, interim) and SYSTEM find the
  work done, a later report is appended once, a no-session service subject is refused,
  record validation; sealed boot inputs refuse direct writes for the administrator and
  the interim session (existing and new names, declared driver files), stay readable;
  SETTING for administrator, interim and SYSTEM, EPERM for standard users, Guest and a
  no-session subject, validation of every format and the record, 24,575-byte HWPROF.CFG;
  `C:\SYSTEM\SYSBCKUP` refuses direct access. 1,869 checks.
- **H07 Registry Checker** (`tests.host.security.test_registry_checker`, new: production
  broker, `checker.c` and the `regcheck.c` client against a real fixture drive; fake
  kernel binding): LIST/BACKUP/RESTORE/FIX EPERM for standard user and Guest; DAILY from
  Guest, standard user, interim and SYSTEM, once a local day by the broker's clock and
  zone; administrator backup with the broker's started flag and checked comment;
  RESTORE recorded and applied at the broker's next start (roots at a higher generation,
  hive slots, BOOT.CFG, HWPROF.CFG), once; damaged root restored at start and reported
  by STATUS to every session; FIX for SYSTEM; a hive without a valid slot restored at
  the broker's start only; wire validation, revoked peer, broker
  without Registry Checker.
- **H02** policy matrix: `/System/SysBckup` row (30 trie nodes) and the boot-input names
  (sealed for every subject but SYSTEM; look-alike names and the same names in other
  folders stay PROGRAM).
- **Sysrecovery** (`tests.host.test_sysrecovery`): the engine on the machine-registry
  layout (rotation, daily, scan/autorestore, restore incl. hive slots and HWPROF.CFG,
  staged restore and its failure cases, legacy archive, damaged archive, the hive
  restored by `rc_autorestore_start` and left by `rc_autorestore`), ScanDisk text.
- **ABI oracle**: rows for every new constant and record (3,404 checks).
- **Target compiles** (canonical i686 kernel flags with the 4 KiB frame limit; the SDK
  compiler for every changed user-space file, the broker with `RC_LOCAL`): pass; the
  client `regcheck.c` uses the broker transport, the broker's is local only. NetSurf
  `chrome.c` `-fsyntax-only`: the same single diagnostic as the base
  (`NSOPTION_fb_toolbar_layout`, a frontend header outside that include path).
- Suites against the unmodified base (the merge index before this step's changes): no new
  failure in the security (90 vs 89 tests, all OK; +1 is the new H07 test), desktop
  (248, the same 5 libreg link-gap errors), Win32 (338, a subset of the base's failures),
  Setup (PASS), recovery/diagnostics (the same `kernel_test.c` compile error) and kernel
  (the same `test_kernel32` failure) suites; details and logs in
  `build/codex/sec-sysvc2-report.md`.
