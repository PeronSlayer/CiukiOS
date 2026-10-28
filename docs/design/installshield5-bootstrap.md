# InstallShield 5 native bootstrap

The bootstrap is a native PE32 helper admitted by the common kernel
`w32_spawn` path. Both native desktop launches and Win32 CreateProcess
(including WinExec and ShellExecute) reach that path. The helper uses the
ordinary Win32 file/process APIs, then starts the original engine through
NTDLL's existing PE loader. `sdk/win16/ne` remains a structural NE reader;
no NE instructions or Win16 imports execute here.
The helper reserves a normal 1 MiB application stack and initially commits
64 KiB for its bounded metadata/Huffman workspaces, so all frames stay
inside the initial commit without CRT stack probes.

Admission requires a bounded MZ/NE Windows application header, SETUP.INS
and an Intel/Alpha/MIPS/PowerPC `_INST32*.EX_` sibling. The Intel archive
must be present for execution on CiukiOS. Other NE applications retain the
existing invalid-image refusal. `_ISDEL.EXE` (or Startup/IsdelName) uses
the same admission and the native cleanup operation. A copied `_ISDEL.EXE`
also qualifies with a Windows `_delis43.ini` record containing file/main,
exeostype=1 and both alt fields (bounded to 4 KiB). The helper validates
every deletion target again; a filename alone never admits an NE image.

## Research and format

Primary implementation reference: [Deark, installshld.c,
`do_instarch_member` and `de_run_is_instarch`](https://github.com/jsummers/deark/blob/master/modules/installshld.c).
This is an installer-engine archive, distinct from InstallShield cabinets
and InstallShield Z archives. It has an 82-byte header, signature
`2a ab 79 d8 00 01`, and a little-endian member count at 78. Each member
contains u32 compressed offset, u32 compressed size, u32 expanded size,
eight reserved bytes, u16 first-name length and bytes, then u16 second-name
length and bytes. Offsets are absolute. The second names are the runtime
names: INSTALL.EXE becomes a unique `_INSnnnn._MP`, zdata.dll becomes
ZDataI51.dll and WUTL95i.DLL becomes _WUTL951.DLL. BOOT16.EXE is unused.
Validate every directory entry's ranges, names and overlaps, and the three
used members' expanded sizes and DCL end markers before publishing output.
The format has no checksum. Bounds are 16 members, 63-byte leaf names,
16 MiB input files and 8 MiB per expanded member; larger variants fail.

The coding reference is [Mark Adler's zlib 1.3.1 contrib/blast](https://github.com/madler/zlib/tree/v1.3.1/contrib/blast),
locally available in build/downloads/zlib/zlib-1.3.1.tar.gz. DCL has a
literal coding flag (0 or 1), a dictionary extra-bit count (4–6), fixed
canonical Huffman alphabets, overlapping backward matches, and length 519
as the terminator. It is unrelated to PKZIP method 6. Our bounded decoder
adapts blast's fixed tables and canonical decoding; it has explicit error
returns, per-call tables and no setjmp, global initialization or libc.
The original blast license is retained alongside it.

[Revenera's Setup command-line reference](https://docs.revenera.com/installshield/helplibrary/IHelpSetup_EXECmdLine.htm)
documents silent `-s`, response `-f1` and log `-f2`. The vendor's
[Professional 5.x migration notes (User Guide, p. 408)](https://docs.revenera.com/installshield25helplib/pdf/InstallShield2019_UserGuide.pdf)
explicitly distinguish old `-SMS` (keep the launcher alive) and old `-z`
(skip the memory check above 256 MiB) from later semantics. Thus the modern
`-z` script-argument meaning must not be projected back onto IS5. These
references cannot establish IS5's private handoff. Preserve user arguments
without interpreting or claiming to implement the install script's switches.
The private format below comes from the lead's original-media Wine trace
in fx-is5boot.

[Microsoft KB 896458](https://learn.microsoft.com/en-us/troubleshoot/windows-client/application-management/x64-windows-not-support-16-bit-programs)
describes recognition/replacement of selected 16-bit installers on x64.
[otvdm's upstream README](https://github.com/otya128/winevdm) describes both
that replacement mechanism and its general Win16 emulator. This change
uses the narrow replacement approach, not a general emulator. Community
IS5 32-bit replacements exist, but their binaries are not vendored.
[Wine bug 4159](https://www.winehq.org/pipermail/wine-bugs/2006-January/024437.html)
reports a regression spinning at InstallShield startup; [Wine bug
46984](https://bugs.winehq.org/show_bug.cgi?id=46984) concerns an engine
installation error. Neither establishes IS5 private handoff bytes or proves
that replacing a stub completes an installation. Keep bootstrap and
engine-runtime evidence separate. The contemporary
[32-bit replacement instructions](https://community.pcgamingwiki.com/files/file/364-installshield-5-32-bit-generic-installer/)
are corroboration only, not a format specification or vendored input.

## Handoff and lifetime

The helper reads SETUP.INI Startup/IsdelName, AppName, CmdLine,
EnableLangDlg and Mif/Type; SETUP.LID Languages/Default; and system.ini
Boot/Shell. It selects the Intel archive. Its required engine outputs are
written under GetTempPathA with exclusive engine-name reservation. Fixed
handoff/DLL names require one active bootstrap; an exclusive Windows
lock file serializes their lifetime through engine exit.

`%TEMP%\_INS0432.INI` is a raw argument line, with no section, key, newline
or trailing NUL. The reference is
`  -fD:\SETUP.INS  -z1 -cx -x<TEMP>\ -x1D:\ -q10010`.
Append Startup/CmdLine and the caller's argument tail in order. `%WINDIR%`
contains `_isenv31.ini` (environment entries joined with single spaces,
no NUL/newline), `_iserr31.ini` ([SharedErrors], 7068/7063/7064/7110), and
`_delis43.ini` ([fileN], main=engine, exeostype=1, alt0=ZDataI51.dll,
alt1=_WUTL951.DLL). Error text is taken from NE string resources, first
the bootstrap and then its `_SETUP.DLL`, with English fallback text.
On the mounted reference disc all four error IDs are in `_SETUP.DLL`;
none is present in SETUP.EXE. The local test checks presence without
recording their commercial text.

Start the engine with an explicitly empty command line and the source
setup directory as current directory. Wait for its exit, return its exit
status and remove owned temporary bootstrap files. A failed process wait
retains those files for later cleanup. Native _ISDEL waits for the same
handoff lock, reads its cleanup list, waits for matching live engine
processes and retries sharing failures; it only deletes validated engine/DLL
paths in TEMP. No general-purpose deletion instruction is accepted from an
arbitrary INI. The helper remains alive through the engine lifetime even
without -SMS, so waited CreateProcess users observe engine completion.

## Validation boundary

Synthetic tests generate their own archive directory and literal-only DCL
streams, exercise malformed metadata/coding and exact handoff bytes.
The original-media check reads the mounted CD, decodes in memory and
asserts the measured expanded sizes (547840, 53760, 46592).
The local H1 test exposes the CD read-only and runs the authored helper
and original engine using the production PE loader and core DLLs.
Commercial input and decoded bytes must not persist in fixtures, image
payloads or diagnostic logs. Local H1 scratch outputs are removed on exit.
No QEMU or canonical image build is part of this implementer validation.

## Engine imports outside the core (round 2, 2026-10-27)

An in-memory import audit of the three decoded members against the core
export tables found three names the Half-Life engine binds statically and
the core did not export; the loader refused the engine with
`missing-export:GlobalCompact` before any engine code ran. The helper and
archive were correct. All three are now core exports:

- `kernel32!GlobalCompact` with its Win16 compatibility family
  (`LocalCompact`, `LocalShrink`, `GlobalFix`/`GlobalUnfix`,
  `GlobalWire`/`GlobalUnWire`). There are no discardable segments to
  compact; compaction returns the process heap's largest free block, as
  [ReactOS `heapmem.c`](https://github.com/reactos/reactos/blob/master/dll/win32/kernel32/client/heapmem.c)
  does through `RtlCompactHeap` (Wine 10.0 `dlls/kernel32/heap.c` returns 0).
  Fix/Wire lock the block and Unfix/UnWire unlock it, as both do.
- `advapi32!RegConnectRegistryA/W`
  ([Microsoft reference](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regconnectregistrya),
  Wine 10.0 `dlls/advapi32/registry.c`): NULL, empty or the local computer
  name (with or without `\\`) opens the predefined root through
  `RegOpenKeyExA(hKey, NULL)`; w32srv answers that with the root's own
  handle. CiukiOS has no remote registry service, so any other computer is
  ERROR_BAD_NETPATH (53).
- `winspool.drv!EnumPortsA/W`
  ([Microsoft reference](https://learn.microsoft.com/en-us/windows/win32/printdocs/enumports)):
  see `printing-win32.md`; the native queue's `/dev/lpt0`..`/dev/lpt2`.

No reused Wine DLL imports any of these names (`config/wine-upper.json`), so
the upper re-audit after the export-surface change has no semantic delta.

The engine also reaches four W21 stubs that trapped (`__builtin_trap`) or
failed where Windows 98 succeeds; it terminated with STATUS_NOT_IMPLEMENTED
at `FreeSid` right after the destination page. They are now implemented:
`advapi32!FreeSid` (NULL succeeds; nothing in this ADVAPI32 allocates a SID,
since `AllocateAndInitializeSid` fails as on Windows 9x, so any other
pointer is returned, the documented failure) and `EqualSid` (byte compare
of valid SIDs, last error cleared, as Wine 10.0 kernelbase; invalid SID is
ERROR_INVALID_SID); `kernel32!lstrlenW` with `lstrcpyW`, `lstrcatW` and
`lstrcpynW`, which Windows 98 KERNEL32 implements; and
`user32!GetWindowPlacement` from USER's own state (no separate restore
rectangle exists: rcNormalPosition is the current rectangle in work-area or
parent client coordinates, min/max positions (-1,-1)). The remaining
ADVAPI32 imports (service manager, tokens, privileges) are returning W21
failures with ERROR_CALL_NOT_IMPLEMENTED, the Windows 9x behaviour the
engine handles.

## Original-media validation in H1 (round 2)

The local test runs `D:\SETUP.EXE` from the mounted CD in the H1 installer
harness. Harness facts established in round 2:

- The CD view must also map handles back to D: (`handle_path`), or
  `GetFileSize` on the stub fails and the helper shows its error box.
- GDI windows publish DESK_DAMAGE, not DESK_V3_PRESENT; the stand-in
  desktop now records their frames as `kind=gdi` with a separate counter
  (lease/GL numbering unchanged, no pixels written).
- The first dialog is an unowned MessageBox, which the desktop protocol
  does not mark modal (DESK_V3_MODAL requires an owner). Dialogs are
  identified by w32srv's own WINDOW_ALLOC record of the WC_DIALOG atom
  (#32770 = 0x8002) and mapped to the wire window by DESK_V3_WIN32_HWND.
- `Services.close` kills every live child before joining service threads:
  the helper's thread serves an INFINITE wait on the engine.
- Spawned bare image names follow the kernel's search order
  (`src/kernel/proc/win32/process.c`); the engine launches the game by
  name after setting its current directory to the install folder.
- An opt-in driver (`defaults=IDLE_MS,MAX answers=...`) presses Enter on
  idle dialogs, or a mnemonic for the Nth prompt, like the guest operator's
  `defaults` action.

Half-Life's setup script runs Sierra's `[System Test]` from `Sierra.inf`
(screen, colours, memory, Windows version, wave, MIDI, processor). In H1
there is no sound device: winmm reaches the audio device through
`int 0x80`, which the H1 confinement refuses for PE code. `sndPlaySoundA` of
the test WAV therefore fails and the script asks three questions; the third
("continue?") defaults to No. The opt-in test answers it with `&Yes`. With
a sound card (the target hardware) the WAV plays. The fixture volume
reports 128 MiB free, which is too small for the title, so the opt-in test
reports 4 GiB free for C: (`GetDiskFreeSpaceA` keeps the Windows 98 2 GB
clamp). With those two harness facts the install completes in H1: support
extraction, welcome, licence, destination, the copy of about 407 MB from
`data1.cab`, Start-menu DDE, the closing pages, engine exit 0 and helper
exit 0. The engine's final launch of the game is the first point beyond the
install; there the game's own imports stop it (gdi32 `GetBoundsRect` for the
executable; kernel32 `OpenFileMappingA` and user32 `WindowFromDC` for the GL
renderer DLL). That is Half-Life runtime compatibility, outside the
bootstrap and installer path.

## Desktop launch routing (fx-autorun, 2026-10-27)

A QEMU run of the canonical image with the Half-Life CD showed that the
desktop never reached the admission above: `desk_w32_identify`
(`apps/desktop/win32_launch.c`) sent every MZ image without "PE\0\0" to the
DOS Box, so `D:\SETUP.EXE` opened a FreeDOS prompt. The desktop now
classifies MZ images as the Windows loader does (`apps/desktop/exe_kind.h`,
Wine 10.0 `server/mapping.c` `get_image_params`): PE and NE with target OS 2
(Windows) go to the Win32 personality; NE for OS/2/BOSS, LE/LX and plain MZ
stay MS-DOS programs. Start > Run, Files/My Computer, desktop shortcuts, the
Startup folder and AutoPlay all use it.

The native Win32 launcher (`apps/desktop/win32_launcher.c`) used to pass the
POSIX path (`/mnt/d/SETUP.EXE`) as argv[0], which becomes the image path and
the admission's `source`. The kernel loader namespace reads an absolute
`/...` name relative to the current drive (`w32_loader_open`), and the helper
starts in the program's folder (D:), so `SETUP.INS` would have been looked up
as `/mnt/D/mnt/d/SETUP.INS`, and the bootstrap would have derived
`-f/mnt/d/SETUP.INS`. The launcher now names the image in Windows form
(`D:\SETUP.EXE`, `C:\...` for the root namespace; `desk_w32_windows_path`) in
argv[0] and in the command line, as Explorer starts programs; the kernel
still opens the native path. A program started that way also sees its own
drive in GetModuleFileName, and the kernel's CreateProcess search for a bare
name tries the caller's image folder first (`src/kernel/proc/win32/process.c`
`executable`), so a disc's autorun program that starts `SETUP.EXE` by name
gets the full `D:\SETUP.EXE` the admission needs (with the POSIX image path
that first candidate missed and the resolved name stayed relative, which
the bootstrap refuses).

Any other 16-bit Windows program is refused by `w32_spawn` with the new
reason `W32_REFUSE_WIN16` (`[win32] load check=win16-application error=193`,
`[win32] refuse win16`), shown by the desktop as "NAME is a 16-bit Windows
program. It needs 16-bit Windows support, which this version of CiukiOS does
not include." (Windows 98 runs these in its Win16 subsystem; CiukiOS's Win16
subsystem follows the 1.0 Beta.) A DLL started as a program is now refused
with the ordinary "not a valid Win32 application" dialog as well, so a spawn
that fails with `-ENOEXEC` is always explained to the user.

## The disc menu in a profile session (fx-hlis5, 2026-10-28)

A QEMU run of image c02b6731 (integration 8) started the setup as a user
does: AutoPlay, the disc's own menu `AUTORUN.EXE`, its install item. The
kernel admitted `D:\setup.exe` and started the helper (`[win32] start
pid=11 exe=C:\System\Win32\is5boot.exe`); the helper showed one window titled
with SETUP.INI's `AppName`, hid it at once, and stayed in its modal loop for
minutes: no engine ever started. Three defects, all outside the H1 harness's
earlier model (a Windows 98 `C:\WINDOWS\TEMP`, no STARTUPINFO, direct
`D:\SETUP.EXE`):

1. **No TEMP folder in a fresh profile.** A logged-on session's Win32 TEMP
   and TMP are `<profile>\Local Settings\Temp` (sec-user-05,
   `sdk/win32/services/start.c`), but no account skeleton creates that
   folder (`apps/accounts/owner_kernel.c` `skeleton[]`); only native
   `tmpfile` and `AllocConsole` create it on demand. The helper's
   `CreateFile(<TEMP>\_INSnnnn._MP, CREATE_NEW)` failed with
   ERROR_PATH_NOT_FOUND and it raised its 7063 box ("unable to create a
   temporary file"). On Windows 2000/XP every profile starts as a copy of
   Default User, which holds `Local Settings\Temp` (Microsoft, [User Profile
   Structure](https://technet.microsoft.com/en-us/library/cc775560.aspx));
   [GetTempPath](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppatha)
   does not verify that the folder exists, and programs rely on it.
   Decision: the session desktop creates the missing components of TEMP
   below the committed profile root when it loads the profile
   (`apps/desktop/profile.h` `profile_temp_ready_at`, called from
   `desk_logon_profile`), owner-only, for new and existing profiles; it
   refuses anything outside the root, `..` and a file in the way.
2. **The error box was hidden by the caller's STARTUPINFO.** `AUTORUN.EXE`
   calls `ShellExecuteA(…, "open", "setup.exe", NULL, NULL, SW_HIDE)` for
   its install item (its other items pass SW_SHOWNORMAL); the kernel puts
   that STARTUPINFO into the helper's parameters, as for any child. USER
   applied a supplied `wShowWindow` to the first ShowWindow of *any*
   top-level window, so the helper's unowned error box was hidden: an
   invisible modal loop that looked like a hang. Windows applies the
   override only to a window it can take for the main one: top-level,
   unowned, with WS_CAPTION, not system-modal, shown with SW_SHOW or
   SW_SHOWNORMAL, once; SW_SHOWDEFAULT always asks for it (Raymond Chen,
   [When does STARTF_USESHOWWINDOW override the parameter passed to
   ShowWindow()?](https://devblogs.microsoft.com/oldnewthing/20100301-00/?p=14773),
   rules zero to two; ReactOS `win32ss/user/ntuser/winpos.c`
   `co_WinPosShowWindow` implements the top-level/caption/owner/command
   conditions; Microsoft, STARTUPINFO, for SW_SHOWDEFAULT). Decisions:
   `user32 u32_startup_show` follows those rules (a system-modal window is a
   DS_SYSMODAL dialog; MessageBox with MB_SYSTEMMODAL builds one); the
   helper's start-failure box is `MB_SYSTEMMODAL | MB_ICONHAND`, so a
   critical start failure is never hidden by the stub's STARTUPINFO (rule
   two item 5 exists for exactly that case), and the helper writes a record
   first: `[win32] warning check=is5-<step>-<Win32 error> module=is5boot.exe`
   (steps: image, temp-path, setup-ini, setup-lid, system-ini, archive,
   decode, lock, engine-file, support-file, handoff, engine-start, cleanup;
   `w32_diag`, the NTDLL export the core DLLs use).
3. **A TEMP with a space split the engine's operands.** The handoff line
   `-x<TEMP>\` is space-separated; with `...\Local Settings\Temp\` the
   engine extracted its support files to `...\Local\` (seen in H1). A Win16
   stub never sees such a path: its module path is short, and Windows NT
   keeps TEMP/TMP in their short form "for compatibility" (Raymond Chen,
   [Why does the system convert TEMP to a short file
   name?](https://devblogs.microsoft.com/oldnewthing/20041224-00/?p=36893);
   XP's `%TEMP%` is `C:\DOCUME~1\<user>\LOCALS~1\Temp`). Decision: the
   helper uses `GetShortPathNameA` for the temp and source directories it
   hands over (engine, support DLLs, `_INS0432.INI`, `_delis43.ini`), keeping
   the long spelling only when there is no short form. The kernel answers
   with the FAT aliases (`W32_F_FULLPATH` flags 1).

The broader class (Win32 TEMP/TMP of a profile session are long, with a
space, unlike XP's short values) affects any program that splits a TEMP path
on spaces (batch files, other installers); it is left to the lead (the
sec-user-05 environment test H11 fixes the long spelling today).

H1 now models a profile session (`cd_services(profile='temp'|'no-temp')`:
the environment block through H1 RPC 4), the caller's STARTUPINFO flags and
show command for every spawned child (`h1_services.c` passes them, the
service hands them to the child's parameters), FAT 8.3 aliases for
GetShortPathName and alias path resolution (`host_services.short_aliases`,
Microsoft FAT32 specification basis-name and numeric-tail algorithms), and a
pointer click (`click=X,Y,W,H`, `h1_game.c`). Local tests on the owner's
disc (`test_is5boot.LocalEngineTests`): the disc menu, install click and
engine's first dialog in a profile session; the failure box visible and
recorded with a missing TEMP; the opt-in complete install from the disc menu
(166 s, 115 files, about 401 MB, the same as the direct `D:\SETUP.EXE` run).
