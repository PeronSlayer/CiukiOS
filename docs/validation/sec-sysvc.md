# sec-sysvc: SYSTEM services for administrator operations, logs, first-boot uid recovery

Directive: `docs/directives/sec-sysvc.md`. Contracts: `docs/design/user-security-kernel.md`
rule table, "Owner metadata and consistency" and its amendment "SYSTEM services for
administrator operations and logs (sec-sysvc amendment)"; `docs/design/user-security-contract.md`
3.5 (first-boot uid recovery amendment), 6.1, 7.1/7.2 (recovery and driver authority),
8.2 A1-10 (per-user logs and crash reports); `docs/directives/sec-lead-decisions.md` K-Q4, K-Q6.
Input: the sec-own report (with the system volume policy bound, the administrator desktop could
no longer save boot options, install/remove driver packs, read `C:\SYSTEM\LOGS`, and NetSurf and
crash reports could not be written there).

## Research

- **Windows XP event logs are SYSTEM's files reached through a service.** The Application, System
  and Security logs live in `%SystemRoot%\System32\Config\AppEvent.Evt`, `SysEvent.Evt` and
  `SecEvent.Evt`, held open by the Event Log service; Event Viewer and every program read them
  through `OpenEventLog`/`ReadEventLog` and write through `ReportEvent`
  ([Event Logging Security](https://learn.microsoft.com/en-us/windows/win32/eventlog/event-logging-security),
  [Eventlog Key](https://learn.microsoft.com/en-us/windows/win32/eventlog/eventlog-key),
  [Change the Default Event Viewer Log File Location](https://technet.microsoft.com/en-us/library/dd277418.aspx)).
  Access is checked per log: LocalSystem and Administrators read, write and clear the Application
  and System logs; the Security log needs `SeSecurityPrivilege` (administrators); Guests are kept
  out on Windows 2000/XP/2003 (`RestrictGuestAccess`, "Prevent local guests group from accessing
  ... log", [Event Log Policy Settings](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc778402(v=ws.10))).
  Interactive users may read the Application and System logs on XP; CiukiOS keeps the stricter
  kernel rule-table row (standard users: no system-log read) as a documented K-Q6 difference,
  because BOOT.LOG carries hardware identifiers and other users' activity.
- **Crash reports.** XP's Dr. Watson writes `drwtsn32.log` to `All Users\Application Data\
  Microsoft\Dr Watson`, a folder every user may write
  ([LogFilePath Entry](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc776594(v=ws.10))).
  The CiukiOS contract instead binds private crash reports to their owner (rule table "Pagefile,
  disk/security logs and retained crash-register reports": "Per-user crash reports may instead
  be placed in the owning profile"; A1-10: "private persistent crash reports remain bound to
  their owner"; interactive/application logs go to the profile's Local Settings\Application Data).
- **Windows 98 maintenance logs and System File Checker.** ScanDisk appends `SCANDISK.LOG`; System
  File Checker verifies against its baseline, backs the current file up to `Helpdesk\SFC` and
  restores the original from installation media (Windows 98 Resource Kit, "Using System File
  Checker", Tables 27.3-27.5; `apps/sysrecovery/sfc.c` already follows it).
- **Repository comparison.** The kernel already runs administrator maintenance as typed SYSTEM
  services of call 99 and the media broker: `recovery_syscall_subject` checks
  `security_storage_admin` on the retained subject and then runs ScanDisk and the boot-status
  record as the kernel (`security_storage.h`: "Native media/recovery calls are typed kernel
  services, not a raw-I/O grant"). Kernel VFS tables are SYSTEM (`vfs_table_init`); a table bound
  to a subject (`vfs_table_init_subject`, sec-k03) is authorized as that subject; the frozen
  whole-file snapshot `file_image_open_as` reads a path as a given subject through the caller's
  own namespace; `px_rename_locked` replaces a file in one FAT transaction. The new services are
  built from exactly these pieces.

## Inventory: administrator writes and reads under /System (from the code)

Classes from `policy_init` (k03): SECRET = SYSTEM only; SEALED = everyone reads, SYSTEM writes;
PROGRAM = everyone reads, administrators write.

| Operation (code) | Path | Class | Before (policy bound) | Now |
| --- | --- | --- | --- | --- |
| Control Panel, Startup tab "Apply to BOOT.CFG" (`control/main.c` boot_save) | `/System/BOOT.CFG` | SEALED | refused | BOOT_CONFIG service |
| System Configuration, Advanced (`sysrecovery/bootcfg.c` bootcfg_update) | `/System/BOOT.CFG` | SEALED | refused | BOOT_CONFIG service |
| Registry Checker restore of BOOT.CFG (`sysrecovery/regcheck.c`, desktop/terminal/msconfig) | `/System/BOOT.CFG` | SEALED | refused | BOOT_CONFIG service |
| Startup menu, selective startup, ASD enable (msconfig, Control Panel, desktop) | boot-status sector | kernel | call 99 SET/ENABLE (already SYSTEM) | unchanged |
| Device Manager driver-pack install/remove (`control/main.c` pack_install/remove, `driver_pack_io.c`) | `/System/drivers/<package>` | SEALED | refused | DRIVER_PACK service |
| Device Manager catalogue (`dp_catalog`), NDIS gate | `/System/drivers` | SEALED (read) | works | unchanged |
| Control Panel Logs tab (`control/main.c` log_read) | `/System/Logs/BOOT.LOG` | SECRET | refused | LOG READ (MACHINE) |
| Log Viewer (`accessories/logview_model.c`) | `/System/Logs/*`, `/WINDOWS/CRASH` | SECRET / PROGRAM | refused / works | LOG LIST/READ, MACHINE and USER |
| Hardware Report logs (`hwreport/native.c`) | `/System/Logs/BOOT*.LOG` | SECRET | refused | LOG READ (MACHINE) |
| NetSurf exit/fetch records (`netsurf/port/chrome.c`) | `/System/Logs/NETSURF.LOG` | SECRET | refused | LOG APPEND (USER) |
| Native crash reports (`desktop/win32_native.c`, `win32_crash.c`) | `/System/Logs/CRASHn.TXT` | SECRET | refused | LOG REPLACE (USER) `CRASHn.TXT` |
| Win32 crash reports | `/WINDOWS/CRASH/CRASHn.TXT` | PROGRAM | admin only | LOG REPLACE (USER) `WCRASHn.TXT` |
| ScanDisk report (`sysrecovery/scanreport.c`: ScanDisk, desktop start, terminal) | `/System/LOGS/SCANDISK.LOG` | SECRET | refused | LOG APPEND (MACHINE, administrators) |
| System File Checker log (`sysrecovery/sfc.c` sfc_log: msconfig, terminal) | `/System/LOGS/SFC.LOG` | SECRET | refused | LOG APPEND (MACHINE, administrators) |
| System File Checker restore (`sfc_restore`) of sealed files | `/System/VMM.ELF`, `/Programs/setup/*` | SEALED | refused | SFC_RESTORE service (any baseline file) |
| System File Checker update (`sfc_update`), Helpdesk backups | `/System/SFC.DAT`, `/System/Helpdesk/SFC` | PROGRAM | works | unchanged (restore backs up as SYSTEM) |
| System File Checker baseline | `/System/SFC.LST` | PROGRAM (was) | admin-writable | sealed: the restore service's trust input |
| ScanDisk, Defrag, Format, Convert (`files.c`, scandisk) | volume | kernel | call 99 SCANDISK, media broker (already SYSTEM) | unchanged |
| Kernel logs (BOOT.LOG rotation, SELFTEST.LOG, HWCHK.TMP) | `/System/Logs` | SECRET | kernel = SYSTEM | unchanged |
| Hardware profiles, performance (`control/main.c` hw_save, perf_save), accel retry, internet options, game removal markers, settings | `/System/HWPROF.CFG`, `/System/settings/*`, `/System/REMOVE*.PND` | PROGRAM | admin works | unchanged (see decision 5) |
| Registry Checker backup/restore of the registry roots (`regcheck.c`) | `/System/Registry/*.reg` | SECRET | not on images since sec-k06 | unchanged (see decision 4) |
| Desktop daily Registry Checker backup | `/System/SysBckup` | PROGRAM | admin works | unchanged (see decision 6) |
| Setup install/repair, rescue log copy | destination volume in Setup mode | unmanaged | works | unchanged |
| `/System/Settings/Hardware`, `/System/Services`, `/Programs/setup` writers | — | SEALED | image builder only | unchanged |

## Design (as implemented)

1. **Typed kernel SYSTEM services on call 99** (`abi.h` block "sec-sysvc"; `fs/sysvc.c`,
   `fs/sysvc_drivers.c`, `proc/syscalls_sysvc.c`; SDK `ciuki/recovery.h`). Ops 5..8: BOOT_CONFIG,
   LOG, DRIVER_PACK, SFC_RESTORE. Each checks the retained subject first (`sysvc_admin`: live
   administrator, or SYSTEM outside a session), then works on fixed targets through a kernel VFS
   table; caller inputs are read as the caller. No new syscall number (others work in parallel on
   the ABI) and no new service role.
2. **Logs.** MACHINE scope (`C:\SYSTEM\LOGS`): administrators LIST/READ (paging up to 64 KiB per
   call), and APPEND only `SCANDISK.LOG`/`SFC.LOG`; everything else there is SYSTEM's. USER scope:
   `<profile>\Local Settings\Application Data\CiukiOS\Logs`, resolved by the kernel from the
   session's committed profile descriptor and opened with a table bound to the caller's subject
   (folders created as the caller); a session without a profile (only the interim administrator
   session before the second boot) holds its logs in `C:\SYSTEM\LOGS\Users\u<uid>` (SYSTEM-owned,
   listed with the system logs as `Users/u<uid>/NAME`). 8.3 names; APPEND starts over past its
   limit (64 KiB default, NetSurf's rule), REPLACE rewrites (crash slots).
3. **Clients.** Control Panel (boot save, Logs tab, driver packs), System Configuration,
   Registry Checker, ScanDisk and SFC logs and restore, Hardware Report, Log Viewer (system logs
   for administrators, the user's own logs for everyone, Setup log and legacy Win32 crash files as
   files), NetSurf, desktop crash reports. Each falls back to the old file path only on ENOSYS (an
   older kernel) or ENODEV (no managed system volume: Setup media), where the volume is unmanaged.
4. **Driver packs.** Same on-disk format as the importer and the NDIS gate. INSTALL validates the
   sealed manifest and its qualification text, checks the adapter (and, for a qualified package,
   the lead qualification and an idle host), stages `<package>.stage` with `transaction.ndp`,
   copies each file from the caller's folder hashing what it writes (EILSEQ on a mismatch),
   writes `registry.ndp`/`manifest.ndp`, re-verifies with `ndp_verify`, renames into place (a
   shorter name, so FAT renames it within its own slots), rolls back on failure, reuses an empty
   folder left by an interrupted removal and keeps any foreign file. REMOVE writes `remove.ndp`
   before `manifest.ndp` goes, so a cut removal is never loaded and the next request (or install)
   finishes it; the importer's older `<package>.remove` tombstones are finished as well.
5. **SFC restore.** The baseline `C:\SYSTEM\SFC.LST` is now sealed (one policy rule, 29 of 32
   trie nodes); the service finds the path (later lines win), reads `source + path` as the caller
   (frozen whole-file snapshot), checks size and SHA-256, copies the current file to
   `C:\SYSTEM\Helpdesk\SFC`, writes `<path>.sfn` and replaces the file with one native
   `px_rename_locked`.
6. **First-boot uid recovery** (lead decision, OWNER `validate_bindings`): a CREATE below
   `uid_high` passes only when the store's complete history proves nothing was ever published
   (no committed generation or account batch, no floor, candidate or checkpoint, no profile, no
   batch or root transaction in flight, every receipt ABORTED) and an aborted receipt named that
   uid. Contract 3.5 amendment.

## Decisions and findings for the lead

1. **Services are kernel typed calls, not a new user-mode SYSTEM process.** Same model as
   SCANDISK and the media broker; avoids a new role/service id in the frozen sec-k01 table and
   lets the kernel read caller inputs as the caller (a user-mode SYSTEM broker could not without
   an impersonation primitive). Call 99 ops 5..8, records in `abi.h` with ABI oracle rows.
2. **System logs: administrators only.** Stricter than XP for interactive users (K-Q6): the
   rule-table row keeps standard users and Guest out of the system logs. Guests keep their own
   USER-scope logs in their own profile.
3. **Per-user crash reports** use the user's log folder (`CRASHn.TXT`, Win32 `WCRASHn.TXT`), so
   a standard user's crash no longer needs `C:\WINDOWS\CRASH` (administrator-only). Existing
   machine crash files stay readable to administrators in the Log Viewer.
4. **Registry Checker's registry part is unchanged.** It reads and restores raw
   `/System/Registry/*.reg` roots, which images no longer ship since sec-k06 (authoritative
   machine roots are the broker's `/System/Security/Registry/Machine`); its backups therefore
   hold only BOOT.CFG and settings, as on the base before this step. A broker backup/restore
   operation belongs to the registry broker's consumer migration (sec-k06 scope).
5. **Boot inputs that are still PROGRAM.** `/System/HWPROF.CFG` and
   `/System/settings/performance.cfg` (read by the kernel at boot) and the display `accel*.cfg`
   attempt files are administrator-writable directly, as before; the rule table's "files consumed
   as privileged configuration during the next boot" would seal them behind the same BOOT_CONFIG
   pattern. Not changed here (32-node policy bound, Control Panel behaviour unchanged).
6. **Session-start maintenance by the desktop.** The desktop's daily Registry Checker backup and
   startup ScanDisk report run as the session user; with a standard-user session (sec-user-03)
   they are refused (backups) or skipped (report: EPERM). They should become SYSTEM jobs.
7. **Pre-existing failure, not from this step:** `tests.host.win32.diagnostics.test_diagnostics
   ...test_kernel_faults_records_and_io` fails identically on the unmodified base
   (`kernel_test.c:135`, dialog-queue-full count).
