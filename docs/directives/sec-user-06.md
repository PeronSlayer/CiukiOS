# sec-user-06: runtime evidence preparation for the security programme

- **Written:** 2026-10-28 10:45 by the lead. Implementer: Claude Opus agent. Base: branch `wt/sec-integ4` after the sec-sysvc2 and sec-user-04 merges. sec-k08 runs in parallel; reconcile with its census in the report.
- **Binding specification:**
  - `docs/design/user-security-contract.md`, row 9 of the step table and the proposed runner work in the section after it (SU-01..14, round-1 and round-2 cases);
  - `docs/design/test-architecture.md`;
  - `docs/directives/fx-accept.md` and `docs/directives/fx-selftest.md`;
  - `tests/suites/acc-t23.json` and `scripts/test/acceptance.sh`;
  - `docs/directives/sec-lead-decisions.md`.

## Scope

1. **Runtime cases.** Wire SU-01..14 as runtime cases selected at run time through the QEMU fw_cfg test item, never through build variants. Add the cases below:
   - round-1 and round-2 challenge, lock, epoch, STATUS, checkpoint and profile;
   - transition and reboot;
   - launch, roles, batch and seed;
   - protected installer and DLL;
   - DOS short path;
   - restart;
   - console and libreg.

   Each must be runnable on the `qemu-t23` profile.
2. **Acceptance cases in `tests/suites/acc-t23.json`.** This is the owner's trial list. Add:
   - first boot to the CiukiUser desktop (autologon);
   - creating a Standard user from User Accounts;
   - logging off, then on as that user;
   - a Standard user refused when installing into `C:\Programs` (and allowed in `C:\GAME`);
   - a Standard user refused writing HKLM, reading another profile, or saving Control Panel machine settings;
   - Guest;
   - the saver lock;
   - Ctrl+Alt+Del to the trusted screen;
   - restart and shut down from the logon screen.

   Every case asserts on serial records, not only screenshots.
3. **Self-test.** The unattended self-test boot (fx-selftest) reports the hardware-only security figures:
   - KDF timing on the CPU;
   - the synchronous registry backup against the desktop's 5 s timeout (S2-6);
   - lock latency on Stand by and resume.
4. **H14:** host parser and fixture wiring only, for every new case. Record all host commands and results.
   - Reconcile `docs/developer-api.md` (`scripts/gen_api_reference.py`) and the supersession pointers in the design documents.
   - Update the inventory rows in `docs/design/os-feature-inventory.md`. A row stays partial until its runtime evidence exists.

## Acceptance

- H14 passes, and `python3 scripts/gen_api_reference.py --check` passes.
- The acceptance runner parses every new case; `scripts/test/acceptance.sh --list` or its equivalent lists them.
- The kernel, security, desktop (`-t .`), Win32 and Setup suites gain no failure against the base.
- The lead runs the canonical build, the QEMU acceptance and the hardware.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-user06` (branch `wt/sec-user06`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Never write into the main checkout.
- Report to `build/codex/sec-user06-report.md` in the worktree. Leave the changes uncommitted.
