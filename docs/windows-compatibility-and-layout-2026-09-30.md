# Windows compatibility and disk layout after M4

**Status: groundwork only.** CiukiOS 0.8.0 runs tested DOS programs in
separate desktop VMs. It does **not** run Windows 95/98 PE programs or
installers. Windows 3.1 support was removed from the current image.

| Today | Next requirement |
| --- | --- |
| DOS COM/EXE launch in M4 windows. | Broader game, input and clean-close coverage. |
| Persistent CiukiOS Settings Registry. | A Win32 registry API bridge and key/handle semantics. |
| PE32 header detection and a clear unsupported message. | PE loader, DLLs and Win32 execution environment. |
| `C:\PROGRAMS` and a TestGames desktop folder. | Application registration and a safe legacy-root migration. |

## DOS games: native execution, QEMU testing

QEMU runs on the development computer to test the disk image; no QEMU binary
runs inside CiukiOS. M4 uses Jemm386 and CVSESSION to give each DOS window a
forked V86 machine, private DOS memory and virtual devices. The x86 DOS code
runs in that environment. Long paths use their FAT short alias at the DOS
EXEC boundary because classic DOS cannot parse VFAT names directly.

`C:\DESKTOP\TestGames` holds COM launchers for DOOM, DOOMVAN and WOLF3D.
Their game data stays under `C:\APPS`; the folder does not duplicate it.
The old DOOMWIN/WOLFWIN desktop-port prototypes were removed from this
folder because their adapter does not work in M4.

| QEMU result | Evidence |
| --- | --- |
| DOOM launches from TestGames while Files remains open. | `build/tests/testgames-final-folder/` and `build/tests/post-m4-three-games-m4/report.json` |
| Wolf4GW reaches its sign-on screen but can lose desktop mouse input; Ctrl+Esc and close do not recover it. | `build/tests/wolf-close-ps2-isolation/` — **open defect**, so Wolfenstein is not verified playable or safely closable in a window. |

## CiukiOS Settings Registry

Control Panel includes an original CiukiOS key/value editor. The private
`C:\SYSTEM\CONFIG\REGISTRY.DAT` format uses checksummed append records for
HKCU, HKLM, HKCR and HKU paths. Users can read, write and delete `SZ`,
`DWORD` and `BINARY` values. A QEMU save/reboot/read/delete test is in
`scripts/qemu_test_registry.py`.

A future Win32 process must use an API bridge; it must not write this private
file. Key enumeration, access controls, multi-user hives, Win32 handles and
`ADVAPI32.DLL` entry points are still missing.

## Free PE32 probes and missing runtime

The project-owned GPLv2 source `src/probes/win32/probe.c` builds two Windows
4.0-subsystem fixtures:

| Probe | First execution milestone |
| --- | --- |
| `HELLO.EXE` | PE load, `KERNEL32` console output and process exit. |
| `SETUP.EXE` | Create a program directory/file and save an HKCU string through `ADVAPI32`. |

Build them with `bash scripts/build_win32_probes.sh build/tests/win32-probes`.
Both work under **host Wine** as fixture checks. In CiukiOS QEMU, the PE
header is recognized and the program gets an unsupported-format message
without a guest fork. The [PE status gate](validation/2026-09-30-post-m4/README.md)
checks that bounded behavior; it is **not Win32 execution**.

To run Windows 95/98 installers or games, CiukiOS still needs:

1. A PE32 loader, DLL loading, a 32-bit process/thread/handle model and the
   `KERNEL32` and `ADVAPI32` surfaces used by the free probes.
2. `USER32`/`GDI32` for windows and drawing, then COM/OLE and installer
   services where a named application actually requires them.
3. Game APIs such as DirectDraw/DirectSound, tested against named free games.

Some old installers use NE bootstraps or proprietary unpackers; each needs
its own compatibility test. See the [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[registry functions](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-functions)
and [Windows Installer](https://learn.microsoft.com/en-us/windows/win32/msi/windows-installer-portal)
references for the API scope.

## Directory roles

| Path | Role |
| --- | --- |
| `C:\SYSTEM\APPS` / `C:\SYSTEM\CONFIG` / `C:\SYSTEM\TEST` | Desktop modules, configuration and optional probes. |
| `C:\PROGRAMS` | Future installed programs; Application Library's Installed page opens it in Files. |
| `C:\DESKTOP\TestGames` | DOS test launchers. |
| `C:\DRIVERS` / `C:\VM` | Driver catalog and VM host/session modules. |

Old root entries such as `APPS`, `DOOMDATA`, `NET`, `SHARE`, `SBEMU`,
`DOS4GW.EXE`, `DOOMSFX.EXE` and `LBA32.TXT` still have build or test
consumers. Moving them needs a coordinated sector-map and path migration.
Application Library cannot index Windows-installed programs until there is
an installer/runtime and a registration contract.
