# Windows compatibility and disk layout after M4

Status: **in progress**. This note records what the current 0.8.0 image can
demonstrate. It does not declare Windows 95/98 application compatibility.

## DOS games and QEMU

QEMU runs on the development computer to test the disk image. CiukiOS does not
start QEMU inside itself. M4 uses Jemm386 and the CVSESSION module to fork a
virtual DOS machine for each desktop DOS window. DOS code runs on the x86 CPU
through virtual 8086 mode, with its own DOS instance and virtual devices.

The desktop `C:\DESKTOP\TestGames` folder contains COM launchers for the three
distinct DOS game tests: DOOM, DOOMVAN and WOLF3D. The older `DOOMWIN` and
`WOLFWIN` binaries are alternate desktop-port prototypes, not separate games;
their launchers were removed because their old adapter cannot run inside M4.
The launchers point to the existing game installations in `C:\APPS`; they do
not duplicate the game data. Files and desktop icons open COM/EXE programs in
M4 DOS windows. A long path is converted to its FAT short alias at the DOS VM
boundary because classic DOS EXEC cannot parse a VFAT long name directly.

QEMU evidence: `build/tests/testgames-final-folder/` shows the TestGames folder and
DOOM launched from it while Files stays open. The full M4 scenario using the
TestGames launcher passed in
`build/tests/post-m4-three-games-m4/report.json`.
An extended Wolf4GW QEMU run reaches its sign-on screen but then stops
receiving desktop mouse input; Ctrl+Esc and the close box do not recover it.
This is an open M4 compatibility defect. The TestGames launcher is present,
but Wolfenstein is **not** verified playable or safely closable in a window.

## Registry substrate

Control Panel includes **Settings Registry**, an original CiukiOS key/value
editor. `C:\SYSTEM\CONFIG\REGISTRY.DAT` holds checksummed append records for
HKCU, HKLM, HKCR and HKU paths. The editor reads, writes and deletes `SZ`,
`DWORD` and `BINARY` values. Its name and layout are CiukiOS's own. The QEMU
save, reboot, read and delete scenario is `scripts/qemu_test_registry.py`.

The on-disk format is deliberately private to CiukiOS. A future Win32 process
must call a compatible API bridge; it must never write this file directly.
The current store does not yet provide key enumeration, access controls,
multi-user hives, Win32 handles, or API entry points in `ADVAPI32.DLL`.

## Free Windows 95/98 probes

`src/probes/win32/probe.c` is GPLv2-licensed, project-owned test source. The
build script produces two 32-bit PE executables with Windows 4.0 subsystem
headers:

| Probe | Required behavior |
| --- | --- |
| `HELLO.EXE` | Load a PE image and use `KERNEL32` console output and process exit. |
| `SETUP.EXE` | Create a program directory and file, and set an HKCU string through `ADVAPI32`. |

Build with `bash scripts/build_win32_probes.sh build/tests/win32-probes`.
Both probes ran under host Wine as a fixture check. That result says nothing
about CiukiOS compatibility. In QEMU, the DOS VM manager recognizes the PE
header and shows an explicit unsupported-format message without forking a
guest. The PE program does **not** run on CiukiOS yet. The QEMU gate
`scripts/qemu_test_win32_status.py` passes both free probes with bounded
close and no guest fork; its captures and report are in
`build/tests/win32-status-three-games/`.

Many Windows 95/98 `.EXE` installers are PE applications that call Win32
APIs; some use older NE bootstraps or proprietary unpackers. Accepting the
filename or adding registry storage cannot make them run. CiukiOS still needs
a PE32 loader, a 32-bit execution environment, DLL
loading, process/thread/handle semantics, `KERNEL32`, `USER32`, `GDI32`,
`ADVAPI32`, COM/OLE, an installer service for MSI packages, and relevant game
APIs such as DirectDraw/DirectSound. The first milestone is the two probes
above; broader game claims require named game and installer test cases.

References: [Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[registry functions](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-functions),
[Windows Installer](https://learn.microsoft.com/en-us/windows/win32/msi/windows-installer-portal).

## Image directory roles

| Path | Role |
| --- | --- |
| `C:\SYSTEM\APPS` | CiukiOS desktop modules. |
| `C:\SYSTEM\CONFIG` | Persistent system configuration, including the registry. |
| `C:\SYSTEM\TEST` | Optional validation programs. |
| `C:\PROGRAMS` | Future installed user applications; shown from the Installed page of Application Library. |
| `C:\DESKTOP\TestGames` | Launchers for DOS test games. |
| `C:\DRIVERS` | Hardware driver catalog. |
| `C:\VM` | VM host and session modules. |

Some existing root entries remain for boot-sector layout and old regression
scenarios. In particular, `APPS`, `DOOMDATA`, `NET`, `SHARE`, `SBEMU`,
`DOS4GW.EXE`, `DOOMSFX.EXE`, and `LBA32.TXT` have references in build and QEMU
test scripts. Moving them requires changing the sector map and all consumers
as one migration; the new directories establish the destination for new work.

Application Library currently contains CiukiOS system applications and an
Installed page that opens `C:\PROGRAMS` in Files. It does not yet index
programs installed by a Windows installer, because those installers cannot run
and no program registration contract exists. The directory layout is an
incremental cleanup, not a completed migration of the legacy root entries.
