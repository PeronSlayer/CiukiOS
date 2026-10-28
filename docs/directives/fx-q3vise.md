# fx-q3vise: Quake III installer hang at the end of setup, and the black screen after the game's lease (T23 trial 6c3b7800)

- **Written:** 2026-10-27 09:50 by the lead. Implementer: Claude agent (Codex weekly quota at 77 %). Base: main 0b9381696.
- **Evidence:** owner's T23, image 6c3b7800 (main 21fa0e975, carrying fx-q3hang). Private log `legacy/local/physical/6c3b7800/disk/LOGS/BOOT1.LOG` in the lead's checkout; read it, never copy it.

## Findings

1. **Installer.** `Q3ADemo.exe` (VISE, pid 7) starts at 13.5 s, shows its wizard, copies the game (the files are on disk), then stops responding: the owner ends it with End Task (`end task dialog kind=not-responding`, `exit pid=7 reason=killed peak_handles=3 threads=1`). The last Win32 records before the hang are `stub kernel32.dll!CreateActCtxW` and `stub kernel32.dll!FindActCtxSectionGuid`. No child process is created, so the fx-q3hang WaitForInputIdle change is not the cause. The end of a VISE install creates shortcuts and Program Manager groups (COM `IShellLink`/`IPersistFile` through ole32/combase, or DDE with PROGMAN) and may start a readme.
2. **Game.** `quake3.exe` (pid 11) gets `present_lease=grant ... mode=274 640x480x32`, the Savage 2D engine reinitializes for that mode, then the screen stays black and the owner has to power off; the log ends a few seconds later. No `[biosvm] step=fullscreen` record precedes this mode set, although fx-biosvm amendment 1 says Win32 leases on flat panels use the BIOS VBE call first. The 3D probe could not qualify on this image (fx-r3dalloc), so Quake III used software OpenGL.

## Scope

1. Reproduce the installer hang on the host: the Quake III installer tests (`tests/host/win32/installers/test_quake3.py`, the H1 host) pass, so find what differs on the image: the DLLs actually used (Wine ole32/combase/shell32 or builtins), the registry (`InprocServer32`, `CLSID_ShellLink`), activation-context stubs returning failure, the rpcss/local-server path of `CoCreateInstance`, DDE broadcast to top-level windows of other processes (desktop, files), or a message loop that never sees a reply. Read the Wine sources under build/downloads (or fetch the matching Wine version) for the exact path after `FindActCtxSectionGuid`. Fix the cause the way Windows 98 behaves, with a host regression that runs the end of the VISE install to completion under the image's DLL and registry set.
2. Explain from the code why the Quake III 640x480x32 lease on a flat panel was set without the BIOS VBE call, whether a black panel follows from that, and fix the policy so a Win32 lease on a panel follows fx-biosvm amendment 1 (BIOS VBE first, native only where proven on that head). Coordinate with fx-biosvm2 (same BIOS VM, larger VBE state area): do not edit `src/kernel/vm/biosvm.c`; if your fix needs it, describe the change in the report.
3. Tests: the Win32 installer, user32, ole32/shell32, display and present suites stay green, plus the new regressions.

Rules: work only in the worktree `/home/peronslayer/Desktop/CiukiOS-wt-fx-q3vise` (branch `wt/fx-q3vise`); never commit, merge, push, run QEMU or build images; run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to build/codex/fx-q3vise-report.md in the worktree. Leave changes uncommitted.
