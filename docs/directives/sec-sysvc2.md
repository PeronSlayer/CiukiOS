# sec-sysvc2: one log/crash location scheme, SYSTEM jobs, sealed configuration writes

- **Written:** 2026-10-28 07:39 by the lead. Implementer: Claude agent. Base: branch `wt/sec-sysvc2` = `wt/sec-integ4` (main + sec-wave3fix + sec-own + sec-k07 + sec-user-05 + sec-user-03) with `wt/sec-sysvc` (10603d7ea) merged; the merge is in progress in the worktree.
- **Binding specification:** `docs/design/user-security-contract.md`, `docs/design/user-security-kernel.md`, `docs/directives/sec-sysvc.md`, `docs/directives/sec-lead-decisions.md`; reports `../CiukiOS-wt-sec-sysvc/build/codex/sec-sysvc-report.md` (and `docs/validation/sec-sysvc.md`), `../CiukiOS-wt-sec-user05/build/codex/sec-user05-report.md`, `../CiukiOS-wt-sec-user03/build/codex/sec-user03-report.md`.

## Scope

1. **Finish the merge.** `apps/accessories/logview_model.c` and `apps/desktop/win32_native.c` conflict. sec-user-05 moved a profile session's NetSurf log and crash reports to `<profile>/Local Settings/Application Data/{NetSurf,Crash}`. sec-sysvc moved them to `<profile>\Local Settings\Application Data\CiukiOS\Logs` and routes the system logs through the call-99 log service.
   - Choose one location scheme. Follow the Windows XP layout (cite it) and make every writer and reader agree on it: NetSurf, the Win32 and native crash writers, Log Viewer, Control Panel's Logs tab, Hardware Report and Dr. Watson-style crash files.
   - Keep the profile-less bootstrap case.
   - Commit nothing. Resolve the markers, then `git add` the two files, leaving the merge for the lead to commit.
2. **SYSTEM jobs.** The desktop's daily Registry Checker backup and the startup ScanDisk report currently run as the logged-on user, so a Standard user's session would be refused. Make them SYSTEM jobs on the call-99 service model, the way Windows XP runs scheduled system maintenance under LocalSystem.
3. **Registry Checker.**
   - It backs up and restores the real registry through a registry-broker operation, instead of the `/System/Registry` files the image no longer ships since sec-k06.
   - It is administrator-only, as the Windows 98 Registry Checker restore and XP System Restore are.
4. **Sealed configuration writes.** Administrators write `HWPROF.CFG`, `performance.cfg` and the display `accel*.cfg` files directly. Move these writes behind the SYSTEM service as the contract requires: Control Panel's display, performance and hardware-profile pages, and whatever else writes them.

## Acceptance

- Host evidence for each item, for an administrator, a Standard user, Guest, the interim session and SYSTEM.
- The kernel, security, desktop (`python3 -B -m unittest discover -s tests/host/desktop -t .`), Win32 and Setup suites gain no failure against an unmodified copy of the base.
- New `abi.h` records get `abi_layout` rows.
- Shared tables, headers and test scripts get one commented block each, listed in the report.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-sysvc2` (branch `wt/sec-sysvc2`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests and `make sdk` / `make CAP= apps` through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Never write into the main checkout.
- Write the report to `build/codex/sec-sysvc2-report.md` in the worktree, and leave the changes uncommitted.
