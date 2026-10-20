# f5-15A2 — installer flows, part A2: pinned NSIS fixture, CAB/INF (`advpack`/`setupapi`/`cabinet`) and self-extractor flows, the `w32-installer-nsis` guest probe

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`
- **Written:** 2026-10-20 by the lead (f5-15A amendment 1 item 3 names this continuation)
- **Worktree:** `../CiukiOS-wt-f5-15a2`, branch `wt/f5-15a2`
- **Base:** f5-15A (partial, merged 2026-10-20: `shell32`/`shlwapi` CiukiOS surfaces, shell folders, `.lnk` writing, `ShellExecute(Ex)`, `SHFileOperation`, temp extraction, child setup launch) and f5-07B (`WININIT.INI [rename]`, `RunOnce`, end-session, virtual restart; `MoveFileEx` delayed operations return 120 as on Windows 98).

## Lead decisions

1. **NSIS pin.** No pinned NSIS archive exists yet. The lead supplies it before the fixture build is attempted: `build/downloads/nsis/` will hold the NSIS 2.51 release (the last 2.x) with its SHA-256 recorded in `config/nsis.json`; the fixture installer is compiled by `makensis` on the host (Linux build of NSIS 2.51 from the same source archive, built by `scripts/build_nsis.py` into `build/tools/nsis/`, like FreeType/TinyGL) from an authored script under `tests/win32/fixtures/nsis/`; the compiled `.exe` is reproducible (recorded hash). Until the archive is present, implement everything else and keep the NSIS fixture build as a clearly reported pending step.
2. The CAB+INF chain and the self-extractor pattern use authored fixtures (f5-15A item 4): an authored INF, an authored CAB built by a pinned host tool (`gcab` or an authored minimal MSZIP cabinet writer in `scripts/win32/`), an authored self-extracting stub.

## Required

1. **`advpack`/`setupapi`/`cabinet` qualification** over the reused Wine builds (f5-20D set): `RunSetupCommand`, `LaunchINFSection(Ex)`, `SetupInstallFromInfSection` for the `[CopyFiles]`/`[AddReg]`/`[DelReg]`/`[UpdateInis]` subset, `FDI` cabinet extraction into the temp folder; the CiukiOS gaps the f5-14A audit flagged are implemented in `sdk/win32/`.
2. **Self-extractor flow**: the authored stub extracts to `GetTempPath`, launches the child setup with a waitable handle, waits, deletes its temp tree (the four self-deleting patterns of f5-15A item 2 are exercised).
3. **NSIS flow** (when the pin is present): install (pages, extraction, file copy, registry `Uninstall` key, `RunOnce` entry, `.lnk` in Start menu and desktop), cancel midway (rollback of partial copies), restart request (`WININIT.INI`/`RunOnce` through f5-07B), uninstall (self-deleting uninstaller). The probe `f5:w32-installer-nsis` emits the four predicates of the suite case `w32-installer-nsis-qemu-t23` (`installer_install`, `installer_cancel`, `installer_restart_request`, `installer_uninstall`, each `errors=0 manifest=1`) from kernel-observed facts (files present, registry values through w32srv's directory, the `.lnk` files, the restart journal); registration in the three tables and the loader model, removal from the known-pending lists.
4. **Tests.** `tests/host/win32/installers/` host harness: CAB extraction checksums, INF section execution on a fixture tree and the registry model, the self-extractor flow with the child wait, the NSIS flow against the compiled fixture when present (skipped with a reason otherwise); `make CAP= apps`, `make CAP= upper` once; the probe dry-run.

## Constraints

No commercial installers, no downloads (report what is missing); `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`. Report `build/codex/f5-15a2-report.md`.
