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

## Amendment 1 (lead, 2026-10-21, after two sessions; WIP commit 68a7a7fc)

Decisions: (1) the native `makensis` adaptation is approved as delivered (exact input pins and Windows stubs, explicit host adaptations). (2) Implement the H1 host-adapter services the four flows need — Win32 file services on a fixture tree, registry through the w32srv model (`tests/host/win32/user32/kernel_bridge.c` pattern), child process creation with a waitable handle, temp folder — so the compiled NSIS, CAB/INF and self-extractor flows run end to end on the host adapter with their manifests checked; fail-closed entry attempts are not acceptance. (3) The i386 PE preflight that the sandbox blocks with SIGSYS stays in the suite; the lead runs it. (4) The four-predicate probe dry-run must pass against the authored fixtures. Report in the "Amendment 1" section.

## Amendment 2 (lead, 2026-10-21, from the lead's run of the installer suite outside the sandbox; log `build/lead/installers.log` in this worktree)

The three tests the sandbox could not run fail on the host adapter; fix production (or the H1 backend where a service is missing), not the expectations:
1. `test_four_predicate_probe_dry_run`: the install flow does not produce `C:\WINDOWS\Desktop\Ciuki Installer Fixture.lnk` (`FileNotFoundError` in `flow_driver.py:75`): the compiled NSIS flow's desktop shortcut is not written (shell-link path through the f5-15A writer) or the manifest names the wrong location.
2. `test_reused_fdi_exact_extraction_and_bad_checksum`: the `cabinet.dll` extraction fixture ends `errors=1` with `kernel_enosys=8`: eight kernel operations the H1 backend answers ENOSYS; identify them from the record stream and implement them in the backend (or in production if a real service is missing) so FDI extraction completes with exact checksums.
3. `test_reused_inf_chain_and_selfextract_child_wait`: after `advpack.dll` attaches, `[win32] crash exception unhandled`, `TERMINATE 3221225477` (access violation): debug the INF chain/self-extractor path to the faulting call (crash record, stack words) and fix the cause.
Rerun `python3 -B -m unittest discover -s tests/host/win32/installers -t .` as far as the sandbox allows and report in "Amendment 2".

## Amendment 3 (lead, 2026-10-21): remaining failures and the merge of current `main`

Outside the sandbox (logs in `build/lead/` of this worktree): (1) `test_four_predicate_probe_dry_run` still misses `C:\WINDOWS\Desktop\Ciuki Installer Fixture.lnk`; (2) `test_reused_inf_chain_and_selfextract_child_wait`: `infchain.exe` terminates with 22 (expected 0) — follow its record stream to the failing step; (3) `tests/host/test_desktop_gate.py` (`test_native_desktop_gate_loop`, `test_native_handshake_reports_through_kernel_hook`): the desktop no longer compiles in the gate harness: `clang: error: linker command failed with exit code 1 (use -v to see invocation) clang: error: linker command failed with exit code 1 (use -v to see invocation) `. The lead also merged current `main` (f5-21b shared fixture gate, f5-07D, f0-49c, f1-86, f3-52, f2-86) into this worktree and left conflict markers in: apps/desktop/build_desktop.py apps/desktop/desktop.c apps/desktop/gate.h scripts/test/loader_model.py scripts/win32/builtins.py src/boot/ciukldr/menu.inc src/kernel/probes/f2_probes_desktop.c src/kernel/probes/selector.c tests/host/test_runner_selectors.py tests/suites/f5-core.json . Resolve them (the installer probe uses the shared gate `GATE_W32_FIXTURE`, single registrations, result page `0x100f0000`), fix (1)–(3), then run the installer suite, `test_desktop_gate test_runner_selectors test_boot_modes`, `tests.host.win32.test_kernel_core`, `test_probe_registry`, `make CAP= apps` once. Do not commit; report in "Amendment 3".
