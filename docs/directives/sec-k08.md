# sec-k08: coverage and bounded-cost audit of the security programme

- **Written:** 2026-10-28 10:45 by the lead. Implementer: Claude Opus agent. Base: branch `wt/sec-integ4` after the sec-sysvc2 and sec-user-04 merges.
- **Binding specification:**
  - `docs/design/user-security-contract.md`, row 8 of the step table: sec-k08 — coverage and bounded-cost audit;
  - `docs/design/user-security-kernel.md`;
  - `docs/directives/sec-lead-decisions.md`, including S2-4, S2-5, U4-3, U4-5 and U4-6;
  - the reports of sec-k01..sec-k07, sec-user-02..sec-user-05 and sec-sysvc/sec-sysvc2, under each worktree's `build/codex/` and in `docs/validation/`.

## Scope

1. **Census.** A source-derived census of every published syscall selector, call-99 service operation, channel service/op (`max_op` in `src/kernel/proc/channel.c`), ioctl and VFS action, with its policy and the test that covers it.
   - A test fails when a new selector has no mapping.
   - Unknown selectors deny.
   - Find and fix the gaps.
2. **Adversarial host harness H13.** Cover the frontends: native, Win32 personality, DOS VM, libreg/registry broker, LOGON/ACCOUNT channels.
   - Each case is a Standard user or Guest trying to:
     - reach SYSTEM state;
     - reach another user's profile, registry hive, crash logs, processes, tokens or windows;
     - write the sealed boot/service set.
   - Every case is refused. Each refusal leaves no fd, subject, pin, page or VM leak, and teardown leaves none either.
3. **Bounded cost.**
   - Show that warm fd checks add no storage lookup.
   - Measure the metadata at 32 profiles and 256 creator-owned roots per volume, the group/session/process limits, and every final-reference failure.
   - Write the figures into a validation document. The PBKDF/KDF Pentium III timing remains a lead runtime gate; prepare the self-test hook that prints it.
4. **Follow-ups decided by the lead:**
   - **S2-5:** seal `clock.cfg` behind call-99 SETTING.
   - **S2-4:** Registry Checker backups include the per-user hives of the profiles, within the 4 MiB archive bound, or document the measured reason they cannot.
   - **U4-5:** the ACCOUNT service refuses a Standard user's rename of their own account.
   - **U4-6:** machine-wide power settings (hibernation, the power-button action) move to a machine file written through the SYSTEM service; per-user schemes stay per user.
   - **U4-3:** decide, with sources (APM 1.2, ACPI 1.0b/2.0 for the hardware baseline), whether a firmware-initiated resume needs a kernel hook to raise the LOGON lock. Implement it if it does.

## Acceptance

- H13, RK, RFS, RW32 and the complete security host harness pass.
- The kernel suite (`bash scripts/test/host_kernel_tests.sh`), the security, desktop (`python3 -B -m unittest discover -s tests/host/desktop -t .`), Win32 and Setup suites gain no failure against an unmodified copy of the base.
- New `abi.h` records get `abi_layout` rows.
- Shared tables, headers and test scripts get one commented block each, listed in the report.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-k08` (branch `wt/sec-k08`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests and `make CAP= sdk` / `make CAP= apps` through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Never write into the main checkout.
- Report to `build/codex/sec-k08-report.md` in the worktree, with the decisions you need. Leave the changes uncommitted.
