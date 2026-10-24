# Startup menu, ScanDisk, Registry Checker and recovery tools (G14)

- **Written:** 2026-10-23 by the G14 implementer (directive
  `docs/directives/g13-g16-os-gaps-batch-3.md`, item G14; scope and rules of
  `g7-g12-os-gaps-batch-2.md`).
- **Inventory rows:** BT-2, BT-3, BT-7, BT-8, BT-10, BT-11, BT-12, ST-9,
  SY-10 of `docs/design/os-feature-inventory.md`.
- **ABI:** system call 99 `CIUKI_SYS_RECOVERY` (the next free number after
  main's highest, 98 `CIUKI_SYS_FILE_INFO`; 92–95 stay reserved for F6), boot
  flag bits 15–31, the raw boot-status sector.

## 1. Windows 98 reference behaviour

Sources are the Windows 98 Resource Kit (owner's media, extracted read-only
to `build/rskt`; chapter files named below) unless another link is given.

| Topic | Windows 98 behaviour | Source |
| --- | --- | --- |
| Startup menu | Hold Ctrl while the computer starts (F8 still works, the 2-second delay is gone); MSConfig ▸ Advanced ▸ "Enable Startup menu" shows it by default. Choices: Normal; Logged (`BOOTLOG.TXT`); Safe Mode (also F5); Step-by-Step Confirmation (also F8 in the menu); Command Prompt Only; Safe Mode Command Prompt Only (also Shift+F5); Previous version of MS-DOS (F4, only with `BootMulti=1`). | `wrkc05_8.htm` "Using Startup Menu Options", Table 5.15 |
| Failed start | "Windows 98 automatically initiates Safe Mode if it detects that system startup failed (for example, if a `Wnbbotng.sts` signature file still exists in the Windows directory)", or the registry is corrupted. | `wrkc05_8.htm` "Safe Mode" |
| Safe Mode | Bypasses startup files and the registry's `[Boot]`/`[386Enh]` processing; loads only mouse, keyboard and standard VGA drivers; the disks stay usable so the configuration can be repaired. Safe Mode Command Prompt Only loads `Command.com` only. | Table 5.16 |
| Step-by-Step | Each part of the start is confirmed with `[Enter=Y,Esc=N]`: registry, startup log, `Config.sys`, `Autoexec.bat`, "Run win.com", "Load all Windows drivers?" (No = Safe Mode). | `wrkc05_8.htm` "Step-by-Step Confirmation" |
| ScanDisk at start | VFAT clears the clean-shutdown bit of the reserved FAT entry while writing and sets it at a proper exit; at the next start, if it is clear, ScanDisk checks the drive. The hard-error bit cleared starts a surface scan. `AutoScan=0` in `Msdos.sys` disables the check, `1` (default) shows a message before ScanDisk starts. | `wrkc10_6.htm` (FAT32 reserved FAT entry), `wrkc10_9.htm` "Disks and Tape Drives" |
| ScanDisk program | Standard (files and folders) or Thorough (also the disk surface); "Automatically fix errors"; lost allocation units are recovered as files with a `.chk` extension; cross-linked files; long names; directory tree; bad sectors. Check Now on the drive's Tools tab. | `wrkc10_6.htm` "Using ScanDisk", `wrkc27_3.htm` |
| Registry Checker | Runs at every start; backs the registry and configuration files up once a day after a successful start into `RB0nn.CAB` in the hidden `\Windows\Sysbckup` (`MaxBackupCopies=5`, oldest deleted); restores the newest good backup when the registry is damaged; `ScanReg /Restore` at a command prompt lists the backups by date; `/backup`, `/comment=`, `/fix`; ScanRegW cannot restore. | `wrkc27_2.htm` "Using Registry Checker", Tables 27.8–27.10 |
| Automatic Skip Driver | Detects devices and operations that stop Windows from starting; "After two failed attempts to load, ASD disables the device driver or stops the operation"; Asd.exe lists them and re-enables them. | `wrkc27_2.htm` "Using Automatic Skip Driver Agent" |
| System File Checker | Scans system files against the baseline `Default.sfc` (CRC, date, size, version); for a changed file: update the verification information, restore the original (backing the current file up to `\Windows\Helpdesk\SFC`), or ignore; deleted-file check; a log appended or overwritten. | `wrkc27_2.htm` "Using System File Checker", Tables 27.3–27.5 |
| System Configuration | General tab: Normal, Diagnostic and Selective startup; per-file and Startup tabs with check boxes; Advanced: "Enable Startup menu", "Disable ScanDisk after bad shutdown", VGA 640×480, etc. | `wrkc27_2.htm` "Using the System Configuration Utility", Tables 27.6–27.7 |

Microsoft's FAT specification (`fatgen103`, "FAT Type Determination",
"FAT32 FSInfo", "FAT[1] ClnShutBitMask 0x08000000, HrdErrBitMask
0x04000000") gives the on-disk rules the ScanDisk engine follows; the
existing kernel FAT code (`src/kernel/fs/fat.c` `cleanbit`/`errorbit`)
already uses the same masks.

## 2. CiukiOS mapping and decisions

### 2.1 Boot status sector (the `Wnbbotng.sts` equivalent)

A file cannot be written by the real-mode loader (it reads FAT32 only), and
a failed start may stop before the kernel mounts C:. The signature is
therefore a raw sector of the system partition's reserved area, like the
black box (`docs/boot.md`, sectors 8 and 32–159): **partition sector 9**,
which `scripts/build_image.py` initializes and Setup's formatter
(`sdk/libdisk/disk.c`, range `DISK_R_RESERVED` 9–31) leaves zero.

Layout `CBS1` (little-endian, 512 bytes; `src/kernel/include/ciuki/bootstat.h`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `CBS1` |
| 4 | 1 | `state`: 0 the last start completed, 1 a start is in progress (written by the loader) |
| 5 | 1 | `version` = 1 |
| 6 | 1 | `disabled`: driver classes turned off by System Configuration (selective startup) |
| 7 | 1 | `skipped`: classes skipped by the Automatic Skip Driver agent |
| 8 | 1 | `active`: classes whose activation is in progress (written by the kernel before each one) |
| 9 | 1 | `request`: bit 0 ScanDisk at the next start, bit 1 thorough, bit 2 show the Startup menu once (Diagnostic startup) |
| 10 | 1 | `failures`: consecutive failed starts |
| 11 | 1 | `last_blamed`: the classes blamed for the last failed start |
| 12 | 8 | `fail_count[8]`: failed starts per class while it was active |
| 20 | 4 | `boots` |
| 24 | 4 | `completed` |
| 28 | 32 | `skipped_at[8]`: the boot number at which ASD skipped each class |
| 60 | 448 | zero |
| 508 | 4 | CRC-32 (zlib polynomial) of bytes 0–3 and 5–507 |

Byte 4 is outside the checksum so the loader can change it without
computing a CRC. The loader never changes any other byte.

**Driver classes** (bit = class number; the same bit order is boot flag bits 24–31):

| Bit | Class | What the kernel skips |
| --- | --- | --- |
| 0 | `display` | the accelerated display driver probe (`accel=0` behaviour) |
| 1 | `sound` | `audio_init` |
| 2 | `network` | `net_init` (NIC driver and NDIS) |
| 3 | `usb` | `usb_init` (BIOS legacy USB stays) |
| 4 | `ports` | serial, parallel, floppy and game port drivers |
| 5 | `power` | APM, the ACPI power owner (the ACPI inventory still runs read-only, as in safe mode) and the platform power driver |
| 6 | `startup` | startup programs (the desktop; consumer added by G21) |
| 7 | — | reserved, zero |

**Loader** (`src/boot/ciukldr/menu.inc`, `bootstat_read`/`bootstat_write`):
after reading `BOOT.CFG`, it reads sector 9 unless a test request is active.
With a valid magic: `state != 0` sets `CBI_F_BOOT_FAILED` and opens the
Startup menu with Safe mode as the default; `disabled | skipped` is ORed into
flag bits 24–31; request bit 0 sets `CBI_F_SCANDISK` (bit 1 also
`CBI_F_SCANDISK_THOROUGH`); request bit 2 opens the menu. After the menu it
writes `state = 1` (INT 13h AH=43h with EDD, AH=03h otherwise), except for
a probe and for Safe mode without disks (no mount could complete that
start). A write error is ignored (the Setup CD's emulated disk is
write-protected).

**Kernel** (`src/kernel/core/bootstat.c`): bound by `storage_add_disk` for
the boot partition right after the black box, never in test requests or
Setup mode. An all-zero sector 9 on a volume with a valid black-box
descriptor is adopted (initialized with `state = 1`); any other content
without the magic or with a bad CRC disables the feature for that boot.
With `CBI_F_BOOT_FAILED` the classes in `active` are blamed:
`fail_count[c]++`, and at two the class is added to `skipped` (and to this
boot's flags, before its activation runs) with `[asd] skip class=<name>
failures=2 boot=<n>`; `failures++`. Without it `failures = 0`. `active`
is then cleared and `boots++`. Before each class activation the kernel
sets its `active` bit and writes the sector synchronously (flushed when the
device cache is on), so a hang inside it is attributed. The end of an
activation only updates memory in the caller; the storage writer task
(`storage_timer`, at most one second later) persists it, so a class that
finished is not blamed for a later failure. The start is **complete** when
the desktop has presented its first full frame (`grants.c`) or the
hardware check page is shown, and no class is active: `state = 0`,
`active = 0`, `failures = 0`, `completed++`, and the classes that ran
forget their failure counts; the writer task persists it. An activation
that begins after completion (the accelerated display probe, the ACPI
thread) writes `state = 1` again and its end restores `state = 0`.

Class activations wrapped this way: sound (`audio_init`), network
(`net_init`), USB (`usb_init`), ports (COM, LPT, game port and joystick
settings, floppy), power (`power_platform_start`, `apm_start`, and the ACPI
inventory thread's power run), display (from `fbdev_accel_schedule` to the
end of the marker worker's attempt). The display class keeps its own
per-driver attempt marker (DM8) as well.

### 2.2 Startup menu (loader)

A normal start keeps the 3-second hint line, now
`Ctrl or F8: Startup menu, S for safe mode, L for log, H for Hardware check`.
During that window, Ctrl held (INT 16h AH=02h, bit 2) or F8 opens the
Startup menu. It also opens without a key after a failed start, when
`BOOT.CFG` contains `menu=1` (System Configuration ▸ Advanced ▸ Enable
Startup menu), or once after "Diagnostic startup" (request bit 2):

```
CiukiOS 1.0.0 alpha Startup Menu
================================

  1. Normal
  2. Logged (boot log on screen)
  3. Safe mode
  4. Step-by-step confirmation
  5. Command prompt only
  6. Safe mode command prompt only
  7. Safe mode without disks
  8. Hardware check

Enter a choice: 1   Time remaining: 30
F5=Safe mode  Shift+F5=Command prompt  Shift+F8=Step-by-step confirmation [N]
```

After a failed start the line `CiukiOS did not start successfully. Safe
mode is selected.` follows the title and the default is 3. The serial log
records the choice as `L:STARTUP=<n>`, then `L:SELECT_SOURCE=menu` as
before. The countdown is
30 s (Windows 98's `BootMenuDelay` default); any key restarts it, digits
select, Enter starts the selected choice, Up/Down move it; F5, Shift+F5
and Shift+F8 start at once. The letters N, S, L, H and P keep their earlier
meaning (`docs/boot.md`, the runner's menu automation). "Previous version of
MS-DOS" has no CiukiOS equivalent and is not shown. The loader's menu
progress deadline grows from 60 s to 5 minutes (stage 0, `progress.inc`) so
a person can read the menu and the step-by-step prompts.

| Choice | Boot flags |
| --- | --- |
| 1 Normal | none |
| 2 Logged | `CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE` (as L) |
| 3 Safe mode | `CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE` |
| 4 Step-by-step | `CBI_F_STEP` plus the answers below |
| 5 Command prompt only | `CBI_F_CMD_ONLY` |
| 6 Safe mode command prompt only | `CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE | CBI_F_CMD_ONLY` |
| 7 Safe mode without disks | `CBI_F_SAFE_MODE` (the earlier S) |
| 8 Hardware check | `CBI_F_HW_CHECK | CBI_F_VERBOSE` (as H) |

Step-by-step prompts, each `[Enter=Y,Esc=N]` (Y keeps the default): "Create
a startup log on screen?" (Y adds the Logged flags), "Load the accelerated
display driver?", "Load the sound drivers?", "Load the network drivers?",
"Load the USB drivers?", "Load the serial, parallel, floppy and game
ports?", "Load the power management drivers?", "Run the startup programs?"
(N adds the class bit to flag bits 24–31) and "Start the desktop?" (N adds
`CBI_F_CMD_ONLY`). "Process the system registry?" has no CiukiOS
equivalent: registry roots are files read by programs, not a startup step.

### 2.3 Safe mode with disks, command prompt only

`CBI_F_SAFE_STORAGE` with `CBI_F_SAFE_MODE` activates ATA and mounts the
disks (activation reason `safe_mode_storage`); every other safe-mode rule
stays (firmware input first, no accelerated video, audio, network, USB,
ACPI power, APM or ports). `safe=1` in `BOOT.CFG`, the `S` key and the
`safe=1` probe suffix keep the earlier safe mode without disks, which the
f1 safe probe and its suites pin. The desktop is spawned with `--safe`
(the four corner labels "Safe mode" and a notice, Windows 98's cue) and,
with `CBI_F_CMD_ONLY`, with `--shell=terminal`. CiukiOS has no text-mode
command interpreter outside the desktop (the Terminal is a desktop
client), so "Command prompt only" is the desktop process with no desktop
icons, no first-start wizard, no Registry Checker run and no startup
programs, and one maximized Terminal that is reopened when it exits; the
top bar stays, for Restart and Shut Down. The Terminal's `WIN` (Windows
98's `win`) turns it into the normal desktop. The supervisor logs
`[supervisor] desktop mode safe=<0|1> shell=<terminal|explorer>` after the
unchanged bootstrap line.

### 2.4 ScanDisk

Engine `src/kernel/fs/scandisk.c` (kernel and FS_HOST): FAT12/16/32 over a
partition block device, independent of the mounted FAT layer.

1. Boot sector and geometry (fatgen103 type determination); FAT entry
   access through a 64-sector write-back window on the active copy.
2. Census: free, used, bad clusters; mirror sectors that differ; the
   clean and hard-error bits.
3. Directory tree (iterative, bounded depth): each chain owned in a
   cluster bitmap; invalid links and cycles end the chain at the last good
   cluster; a cluster already owned by another chain is a **cross-link**
   (recorded with its predecessor); a file whose chain is shorter than its
   size gets its size adjusted; reserved attribute bits are cleared. A
   chain longer than the size is kept: the FAT layer's dirty repair owns
   excess tails (it recovers `BOOT*.LOG` reservations).
4. Cross-links are fixed by **copying** the shared tail for the later file
   (Windows 98's "Make copies").
5. Lost chains (allocated, unowned clusters; heads found with a second
   bitmap, cycles broken) are saved as `\FOUND.nnn\FILEnnnn.CHK`
   ("Convert to files"), or freed when the caller asks for it.
6. Thorough: every data cluster is read; an unreadable free cluster is
   marked bad, an unreadable used cluster is copied (readable sectors) to a
   new cluster, relinked and marked bad. System-area read errors are
   reported only.
7. The other FAT copy is rewritten from the active one where it differs.

The engine never sets the clean or hard-error bits: the FAT layer's own
dirty repair still runs afterwards (log recovery, tail detach) and owns the
clean bit.

**At start:** when the boot volume's FAT[1] says unclean or hard error,
the loader (unless `autoscan=0`) prints `CiukiOS was not shut down
properly. ScanDisk will check your disks. Press Esc within 10 s to skip.`;
`autoscan=2` runs without the message. Esc sets `CBI_F_SCANDISK_SKIP`.
In the kernel, `storage_enable_write` runs the engine with automatic fixes
on every fixed FAT volume whose FAT[1] is unclean or flagged (thorough when
flagged or requested), and on all fixed volumes when the check was
scheduled (`CBI_F_SCANDISK`), unless skipped, `autoscan=0`, a test request
or Setup mode; the volume is remounted from disk before the FAT layer opens
its write session. The report is kept per drive and logged
(`[scandisk] drive=C ...`); the desktop shows it ("ScanDisk Results")
and appends it to `C:\SYSTEM\LOGS\SCANDISK.LOG`.

**On demand:** `/Programs/scandisk` (System Tools, and Check Now on the
drive's Tools tab) runs a check through call 99 with the drive locked and
committed first. A mounted fixed volume cannot be repaired under open
files: when the check finds errors and "Automatically fix errors" is on,
the fix is scheduled for the next start (request bits) and ScanDisk offers
to restart, as Windows NT does for an in-use volume. This is the one
documented difference from Windows 98, whose ScanDisk locks the drive
while Windows runs.

### 2.5 Registry Checker

`apps/sysrecovery/regcheck.c` (desktop, Terminal, System Configuration):
roots `/System/Registry/*.reg`, `/system/BOOT.CFG` and
`/system/settings/*.cfg` are packed into `C:\SYSTEM\SYSBCKUP\RB00n.CRB`
(an uncompressed archive: header `CRB1`, creation time, comment, a
"started" mark, entries with path, size and CRC-32). The desktop runs the
checker once the start is complete: every root is parsed; a damaged root is
restored from the newest backup holding a valid copy and the user is told;
then, if no backup of today exists, one is made and the oldest beyond five
is deleted. `SCANREG` in the Terminal: `/BACKUP`, `/COMMENT="text"`,
`/RESTORE` (numbered list by date, then a restore of every file, then a
restart prompt). Restoring needs no other registry writer, so the
Terminal refuses `/RESTORE` unless the start is "Command prompt only"
(Windows 98: "you must use the MS-DOS-based utility outside of Windows").

### 2.6 System File Checker

`scripts/build_image.py` writes `/System/SFC.LST` (the `Default.sfc`
baseline): one `sha256 size path` line for every payload under
`/Programs` and `/System`, except mutable files (registry, logs, settings,
`BOOT.CFG`, `BUILD.TXT`, the page file and the list itself), from the
same records as `build-manifest.json`. `apps/sysrecovery/sfc.c` reports
changed and deleted files, and per file: update the verification
information (`/System/SFC.DAT` overrides), restore from a source folder
(the current file first copied to `/System/Helpdesk/SFC`), or ignore;
`/System/LOGS/SFC.LOG` is appended. Restoring straight from the Setup CD
needs the El Torito image's FAT view outside Setup mode, which the kernel
does not provide: the source is a folder the user names (System
Configuration's Restore asks for it; `SFC /RESTORE file source` in the
Terminal), and the source file must match the baseline (`EILSEQ`
otherwise).

### 2.7 System Configuration

`/Programs/msconfig` (System Tools): General (Normal, Diagnostic =
Startup menu once at the next start, Selective = one check box per class,
stored in `disabled`), Advanced (Enable Startup menu `menu=1`, Disable
ScanDisk after bad shutdown `autoscan=0`, serial log), Skipped drivers
(ASD list with failure counts and Enable), Tools (Registry Checker,
System File Checker, ScanDisk). The Startup tab (Run keys, StartUp folder)
belongs to G21.

## 3. Interfaces

- Boot flags (`ciuki/boot_info.h`, `boot_info.inc`): 15 `CBI_F_BOOT_FAILED`,
  16 `CBI_F_STEP`, 17 `CBI_F_CMD_ONLY`, 18 `CBI_F_SAFE_STORAGE`,
  19 `CBI_F_SCANDISK`, 20 `CBI_F_SCANDISK_SKIP`, 21 `CBI_F_SCANDISK_THOROUGH`,
  22 `CBI_F_MENU_SHOWN`, 24–31 `CBI_F_SKIP_*` (class bits). The defined mask
  becomes `0xFF7FFFFF`.
- `BOOT.CFG` tokens: `menu=1`, `autoscan=0`, `autoscan=2`.
- Call 99 `CIUKI_SYS_RECOVERY` (EBX operation, ECX record, EDX exact size):
  `STATUS` (everyone), `SET` (disabled classes and requests), `ENABLE`
  (clear ASD skips), `SCANDISK` (online check), `REPORT` (the boot-time
  ScanDisk report of a drive). Mutations need the recovery capability,
  delegated like the time authority to the desktop, Control Panel,
  Terminal, ScanDisk and System Configuration.

## 4. Loader size

The 4 KiB `cluster` buffer moved from the fixed file into loader BSS
(`cluster_read` fills all of it before any reader), as `selector_buffer`
and `sector` did earlier; `loader_end` grows from `0x26841` to `0x27841`
(one more reserved page before reclaim). After the Startup menu,
Step-by-step, the ScanDisk message and the boot-status code the fixed
24,576-byte image has **2,967 free bytes** (`scripts/loader_budget.py`,
1.0.0 alpha version string; it had 775). The menu stage's progress
deadline is 5 minutes (5,461 ticks).

## 5. Tests

Host only (no QEMU): the loader model and the assembled loader run in the
host x86 executor (`tests/host/test_startup_menu.py`), the boot-status
logic (`tests/host/bootstat_test.c`), the ScanDisk engine on generated and
damaged FAT images (`tests/host/fs/test_scandisk.c`), the Registry Checker
and System File Checker, BOOT.CFG options and the System Configuration
model (`tests/host/sysrecovery_test.c` via `test_sysrecovery.py`), the
image builder's sector 9 and `SFC.LST` (`test_image_payloads.py`), the
fx-15 window-model row 31 (Safe mode labels, command-prompt shell), the
Files drive sheet's Check Now (`files_properties_test.c`, frame
`g6-tools`), and the ABI rows.

## 6. Not delivered (gaps)

- The kernel's own dirty-mount repair still frees lost clusters when the
  user skipped ScanDisk (Esc, `autoscan=0`); only the ScanDisk engine
  saves `.CHK` files.
- Removable drives are checked on demand but never repaired by ScanDisk
  while mounted; their mount-time repair is the FAT layer's.
- On-demand fixes of a fixed drive always wait for the next start
  (scheduled); Windows 98 locks the drive and fixes at once.
- No QEMU or hardware evidence: the loader paths ran in the host executor,
  the kernel paths in host harnesses only.
- Startup programs (class 6) have no consumer until G21; the msconfig
  Startup tab is G21's.
- `SCANREG /RESTORE` is refused unless the start is Command prompt only;
  after `WIN` in that start it is still accepted.
