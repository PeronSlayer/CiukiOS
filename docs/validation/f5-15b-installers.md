# F5-15B original installer acceptance: Linux-adapter evidence

Date: 2026-10-21 (evening). Host evidence only: the H1 Linux adapter
(`tests/host/win32/wine/upper_h1.c` + `tests/host/win32/installers/`), the
production PE loader, ntdll, kernel32, the builtin DLL store and the qualified
reused Wine PEs over the H1 file/registry/process/wait services. The QEMU
`installers` tier and the T23 columns are the lead's; they are `not_run`
here. Exact commands, records and the open items are in
`build/codex/f5-15b-report.md`.

## What the adapter can and cannot prove

The H1 adapter runs the original PE on a confined i386 process: the loader
accepts or refuses the image, imports bind against the builtin store, every
file, registry, process and wait request is served by the fixture tree and
the production w32srv registry model. Three kernel operations are ENOSYS by
construction: `W32_SERVICE` (0x200, the desktop service connect that every
window creation needs), `W32_MESSAGE` (0x104, the USER message queue) and
`W32_THREAD` (0xa, thread creation). A run that reaches one of them after a
clean load is recorded as **boundary**: the personality accepted the
installer and served every earlier request; the UI-driven phases (dialogs
and their scripted input, file copy, registry, links, restart, uninstall)
need the desktop and belong to the QEMU tier. A loader refusal, a crash or
an unexpected ENOSYS before that boundary is a failure of the
installer-reachable surface and is listed with its exact record.

The installers carry no documented unattended switch that avoids their first
window (Wise `/S` and IExpress `/Q` still create the progress window;
InstallShield 5 `-s` needs a `setup.iss` recorded on a reference system), so
no engine can complete on the adapter even in silent mode.

## Payload import (lead downloads, SHA-1 verified, SHA-256 recorded)

`python3 -B scripts/win32/workloads.py --import-unpinned` links each flat
download into `build/downloads/win32/<id>/<file>`, verifies the manifest's
size and SHA-1 and writes `<file>.receipt.json` (status
`candidate-unreviewed`: the manifest's `sha256` stays `null` until the lead
promotes the pins; `tests/host/test_win32_workloads.py` asserts that).

| Workload | File | Bytes | SHA-1 (manifest, verified) | SHA-256 (recorded) |
| --- | --- | --- | --- | --- |
| `diablo-shareware` | `DIABLOSW.iso` | 137576448 | `bf1a62b24ce01ce39993955bff4b3c4d4ab9d647` | `ac3bc8196f84c8cf59af502a5318717075adab20f2dc0af42c42d6b643c6a33e` |
| `starcraft-shareware` | `StarCraft Shareware (USA).zip` | 248478379 | `a29ccfb4b34d55034bbae6bde23199fc4766f288` | `a10834d3000462dc333371fd0323206f0c8825c799f757ccbd30a5539b478f93` |
| `aoe-trial` | `MSAoE.exe` | 24786864 | `ccd19bd57e7070ffe7c0b20ed9265d680fbe36c7` | `60e01bd94cc88af64fd2ca40a75f3df783b8ea7725971ddd02c60f4d11e96973` |
| `half-life-uplink` | `hluplink.exe` | 50872079 | `605e110670ca3d55b2093b7750d1d0138cbf1f4c` | `9f236e9785980c127d46870a828ba4dac756308767ebd498b85bd7edff3305a8` |
| `ut-demo-348` | `UTDEMO348.EXE` | 55647232 | `faf2c18852a1a53c59db490e044e0d3e100e8fed` | `59e65276c67af7859935d79001b93934928b3e1c3a8cc300807cdfa95208f708` |
| `worms-armageddon-demo` | `WormsArmageddonDemo.zip` | 13903680 | `5a53c889037b91566c211b16d527c69afd037be8` | `1f7e79cb999314ed402a5b40e381b0cf91242d122fed0e8d769aef0b339ce604` |
| `heroes3-demo` | `h3demo.exe` | 98265058 | `8d44f794a3fe80bea524f90b38fed3325ca3d1cb` | `83cbb6319386d6deba65273f53a60395fda8cce74cc24177b2ac3560ded9c315` |
| `rct-demo` | `RCTYCOON.EXE` | 19596288 | `f985bb782e974572805ee2990cfa2d1fd4e93eee` | `057066729e835c1840a051daf12295f25e05ed8a5bdd64cf79e46373ce42357a` |
| `jazz2-shareware` | `jazz2swf.exe` | 11019236 | `d2ab0a3107360563c5c3bb897a4adfb9d319c04f` | `ce1bfbc7727a6ff03ba461ad5fdc7c292d3b90924baaf6111d7bbfecf7a55231` |
| `ta-demo` | `Total Annihilation.exe` | 21540864 | `ce8a2c21488fbe807cd855b8effcc127053c3e85` | `5e41cf05226c274b4ac9e4398f74f6b321506bd7a4317ee1744ff7aceba34c49` |

Bundled distribution terms were not inspected (`licence.distribution_verified`
stays false); the owner-local reserve (`gta2-freeware`) is never used.

## Engine identification at import (host extraction assists discovery)

| Workload | Engine (observed) | Evidence |
| --- | --- | --- |
| Half-Life: Uplink | Wise Installation System stub, PE32 GUI 4.0 | strings `Initializing Wise Installation Wizard...`, `GLBSInstall`, `STUB32.EXE`, `WiseMain`; 7-Zip cannot list the overlay |
| Heroes III demo | InstallShield 5 self-extracting package | MSZIP cabinet: `Disk1/Setup.exe`, `_Setup.dll`, `_ISDel.exe` (all PE32 4.0), `SETUP.INI` AppName `Heroes of Might and Magic III Demo`, `data1.cab` |
| Age of Empires trial | IExpress/WEXTRACT with ADVPACK and DirectX setup names | MSZIP cabinet (130 entries): `setup.exe` (PE32, "Age of Empires Trial Setup", SETUPCOMMAND/SETUPDATA), `setupenu.dll`; strings `ADVPACK.DLL`, `dsetup.dll`, `dsetup32.dll`, `dsetup16.dll`; no `directx/` folder in the cabinet |
| Unreal Tournament demo 348 | WinZip Self-Extractor (1995-96) + `System/Setup.exe` (Unreal engine setup, imports `Core.dll`, `Window.dll`) | ZIP with 628 entries |
| RollerCoaster Tycoon demo | WinZip Self-Extractor + `Setup.exe` (PE32; `DirectXSetupA`, `DirectXSetupGetVersion` through `DSetup.DLL`; Uninstall key) | ZIP with 82 entries; `AUTORUN.INF` `OPEN=SETUP.EXE -autorun` |
| Jazz Jackrabbit 2 shareware | Epic vendor installer, PE32 | imports `MPR.dll!WNetConnectionDialog`, `VERSION`, `COMCTL32`, `ole32`; strings `DirectXSetupA`, `%sdsetup.dll`, `UninstallString`, `UnInstallLog` |
| Total Annihilation demo | Cavedog vendor installer, PE32 (identified at import) | imports `COMCTL32`, `comdlg32`, `WINSPOOL.DRV`; strings `DirectXSetupGetVersion`, `dsetup.dll`, `Direct X version 5 must be installed`, `C:\Cavedog\TotalA\Demo` |
| Worms Armageddon demo | **InstallShield 3, Win16** (identified at import) | `SETUP.EXE`, `_SETUP.DLL`, `_ISDEL.EXE` are NE images; `SETUP.INI` AppName `Worms Armageddon Demo` |
| Diablo shareware | ISO 9660: `SETUP.EXE` launcher (17 KiB) spawning `AUTORUN.EXE` (53 MB Blizzard installer, MPQ, `DiabloInstall`, `Diablo Uninstaller`); `DIRECTX3/DIRECTX/DXSETUP.EXE` + `DSETUP.DLL` | 817 entries |
| StarCraft shareware | bin/cue, one MODE2/2352 data track (no audio), converted on the host with `bchunk` to `starcraft01.iso`: `SETUP.EXE` launcher spawning `INSTALL.EXE` (163 MB Blizzard installer); `DIRECTX5/` | 582 entries |

Static imports of every entry executable and extracted setup child are in
`docs/design/win32-census/acceptance-installers.txt` (metadata only).

## Acceptance table (Linux adapter; QEMU and T23 columns are the lead's)

Phases: extraction / UI / copy / registry / links / restart / uninstall.
`boundary(x)` = clean load and service up to the adapter's ENOSYS boundary x
(S = `W32_SERVICE` desktop connect, M = `W32_MESSAGE`, T = `W32_THREAD`).
Records are under `build/host/win32/installers/titles/<id>/` after
`python3 -B -m unittest tests.host.win32.installers.test_titles.<Engine>Tests`.

| Engine / title | Variant | Loader | Served before the boundary | Result on H1 | Extraction | UI | Copy / registry / links / restart / uninstall | QEMU | T23 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Wise: Half-Life Uplink | interactive, `/S` | accepted (50 MB image) | own-file open/seek/read ×9, ATOM_ADD, WINDOW_ALLOC/SET/FREE | boundary(S) | not reached (window first) | not_run | not_run | not_run | not_run |
| InstallShield 5: Heroes III demo | interactive, `-s` | accepted (98 MB image) | 375 writes to TEMP (one extracted file), FIRST/NEXT, mkdir/rmdir/delete, 84–87 registry requests, PROGMAN/USER cleanup ops, 1149 message-queue requests | boundary(S then M); exit −10 after the pump | observed | not_run | not_run | not_run | not_run |
| IExpress/advpack: Age of Empires trial | interactive, `/Q`, `/Q /T: /C` | accepted | 13 probes for absent files (error 2), registry OPEN/ENUM, window directory, 2 display queries | boundary(S) | not reached | not_run | not_run (DirectX step: see requirement 2) | not_run | not_run |
| WinZip SE: Unreal Tournament demo | interactive | accepted (55 MB image) | own-file reads, window directory, registry probe | boundary(S) | not reached | not_run | not_run | not_run | not_run |
| WinZip SE: RollerCoaster Tycoon demo | interactive | accepted | same pattern | boundary(S) | not reached | not_run | not_run | not_run | not_run |
| Vendor: Jazz Jackrabbit 2 shareware | interactive | **accepted after the fix** (was refused: `missing-export MPR.dll WNetConnectionDialog`) | 61 registry requests, comctl32 attach | boundary(S) | not reached | not_run | not_run | not_run | not_run |
| Vendor: Total Annihilation demo | interactive | accepted | 61 registry requests, `GetProcessVersion` (now real), comctl32 (`CreateActCtxW` W21 stub) | boundary(S); exit 3 | not reached | not_run | not_run | not_run | not_run |
| CD: Diablo shareware | `D:\SETUP.EXE` | accepted | launcher spawns `D:\autorun.exe` (real child, waitable) | child boundary(T): `AUTORUN.EXE` creates a worker thread after 1994 seeks/2811 reads of its MPQ | n/a | not_run | not_run | not_run | not_run |
| CD: Diablo shareware | `D:\AUTORUN.EXE` | accepted (53 MB) | MPQ reads, 65 registry requests, `winmm!#2` = PlaySound (now exported) | boundary(T) | n/a | not_run | not_run | not_run | not_run |
| CD: Diablo DirectX 3 `DXSETUP.EXE` | `D:\DIRECTX3\DIRECTX\DXSETUP.EXE` | **accepted after the fix** (was refused: `missing-export KERNEL32.dll RtlZeroMemory`) | intercepted before import binding: `[win32] installer dxsetup already-installed-4.07.00.0716`, `RESULT intercepted dxsetup`, `TERMINATE 0`, no file written | intercepted (measured) | n/a | n/a | no write under `C:\System` | not_run | not_run |
| CD: StarCraft shareware | `D:\SETUP.EXE` | accepted | launcher spawns `D:\install.exe` | child boundary(S) | n/a | not_run | not_run | not_run | not_run |
| CD: StarCraft shareware | `D:\INSTALL.EXE` | accepted (163 MB image) | 59 disk-space queries, CHDIR/CWD, 69 registry requests | boundary(S); exit 1 | n/a | not_run | not_run | not_run | not_run |
| InstallShield 3: Worms Armageddon demo | — | — | — | **deferred (Win16)**: NE setup stub measured by the driver | — | — | — | — | — |

No installer wrote anything outside `C:\WINDOWS\TEMP`; nothing was written
under `C:\System`; no `[win32] crash`; the only stubs hit were
`user32!MessageBeep` (every GUI engine) and `kernel32!CreateActCtxW` (the
reused comctl32, documented W21).

## Gaps found and their disposition

| Gap | Found by | Disposition |
| --- | --- | --- |
| `KERNEL32.dll!RtlZeroMemory` missing (DirectX 3 `DXSETUP.EXE` refused at load) | Diablo CD run | fixed: `RtlFillMemory`/`RtlMoveMemory`/`RtlZeroMemory` implemented in ntdll and forwarded by kernel32 as on Windows 95/98 |
| `MPR.dll!WNetConnectionDialog` missing (Jazz Jackrabbit 2 refused at load) | jazz2 run | fixed: the acceptance-installer census feeds the W21 stub set (`stubs.acceptance_extra`); mpr exports it with the LSTATUS failure convention |
| `winmm!#2` imported by ordinal (Diablo `AUTORUN.EXE`) hit an opaque stub | Diablo run | fixed: ordinal 2 = `PlaySound` → `PlaySoundA` (Wine 10.0 `winmm.spec` line 2) |
| `kernel32!GetProcessVersion` W21 stub (Total Annihilation) | TA run | fixed: real implementation from the image's optional header |
| `kernel32!SetConsoleCtrlHandler` W21 stub (MSVCRT startup of IS5 Setup, TA, Diablo) | several | fixed: registration accepted, nothing delivered (no console control source) |
| `DXSETUP.EXE` of DirectX 3 has no version resource: the authored identity parser never matched it | Diablo CD run | fixed: a version-less `DXSETUP.EXE` importing `DSETUP.DLL!DirectXSetup(A)` is the launcher by construction; the resource identity stays authoritative when a version resource exists (unit cases added) |
| `ntdll!_vsnprintf`/`_vsnwprintf` missing: the reused `shlwapi.dll` resolves them in its DllMain (`[win32] load ntdll.dll missing-export`) | every run that attaches comctl32/shlwapi | **open** (W21 decision for the lead): implement both in ntdll (the user32 `wvsprintf` engine is the candidate source, CRT truncation semantics differ) |
| `dsetup!DirectXDeviceDriverSetupA` missing: DirectX 3 `DXSETUP.EXE` resolves it dynamically | Diablo CD run | **open**: ABI not verifiable from in-repo sources (f5-16: unknown ABI never gets a stub); the intercept makes it moot for `DXSETUP.EXE` |
| H1 adapter: 16 MiB image snapshot limit, session PID ≠ PE PID (w32srv WINDOW_SET refused with 87), USER gate numbers unrecorded, server/file/process requests unnamed in the trace | first Wise run | fixed in the test harness only (`h1_services.c`, `host_services.py`, `upper_h1.c`) |
| `[win32] load image newer-subsystem-version` on every attach | all runs | pre-existing warning (f5-15A2 amendment 11), untouched |

## Host suites on this tree

`python3 -B -m unittest discover -s tests/host/win32 -t .`: 340 tests,
1 failure (the pre-existing f5-15A2 amendment-11
`test_original_uninstaller_copies_launches_and_exits`, delegated NSIS
uninstaller copy ending with NSIS exit 2 after a desktop-service connect the
adapter refuses), 3 skipped; `make CAP= apps` green;
`scripts/win32/wine_upper.py refresh-audit` applied for the
`SetConsoleCtrlHandler` change.

## Shims

No `SHIMS.INI` entry was added: no run reached a point where a version,
heap, memory-status or FPU shim changed the outcome, and the contract's rule
(entries only with evidence) applies. The DirectX version reported to
`DirectXSetupGetVersion` (4.07.00.0716) satisfies the Total Annihilation
("DirectX 5 required") and Jazz 2 preflights statically.
