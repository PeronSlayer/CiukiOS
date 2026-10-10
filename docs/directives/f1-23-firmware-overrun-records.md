# Directive f1-23: `firmware_overrun` and `disallowed_io` subcases of `input-fault` from the BIOS VM self-test

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`input-fault` row:
  "Firmware overrun/disallowed I/O cannot mutate physical PIC/PIT";
  evidence: deadlines, errors, overflow count, controller reads by owner,
  quarantine, survivor progress), `device-firmware-ownership.md` (BIOS VM
  monitor, virtual PIC/PIT, I/O policy, quarantine), the f1-15/f1-18/f1-19
  reports on the firmware input backend.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-firmware-records`, from `main` at or after `0fd56fc`.
  Files: `src/kernel/drivers/i8042_probe.c` (the `input-fault` probe),
  `src/kernel/vm/biosvm.c` and `src/kernel/include/ciuki/biosvm.h` only to
  expose report fields the records need, `tests/suites/f1-input.json` (the
  `firmware_overrun` and `disallowed_io` cases and their five-profile
  siblings), `tests/host/*` (predicate tests from the probe's own record
  formatting, the i8042/biosvm host harness), `tests/host/record_scope_test.py`
  allow-list if a new emitter site is added.

## Observed on image `a4806d0c…` (commit `2326aee`, runner cases of `f1-input` on all five profiles, 2026-10-11)

`registry`, `input-fault`, `input-qemu-t23`, `input-qemu-e500`,
`framebuffer` and `framebuffer-no-lfb` PASS. `firmware_overrun` ends
`NOT_RUN` with the suite's own declaration
`not_run_subcases: [{subcase: firmware_overrun, reason:
biosvm_selftest_has_no_record_emitter}]`, `disallowed_io` the same, and the
runner then marks every later case of the suite (the eight
`qemu-desktop-1998/2002` cases) as prerequisite-failed. The kernel has
`biosvm_selftest()` (`src/kernel/vm/biosvm.c:792`) with a complete report
(`policy_result`, `denied_result`, `timeout_result`, `later_result`,
`disallowed`, `timeouts`, `pic_before/after`, `pit_before/after`,
`pic_unchanged`, `pit_unchanged`, `disabled`, `mappings_ok`), but no probe
calls it and nothing emits records for it.

## What to do

1. In the `input-fault` probe, after the existing injection subcases, run
   `biosvm_selftest()` when the firmware backend is available (QEMU and
   SMBIOS-identified laptops; on a boot without the BIOS VM emit a
   `status=not_run reason=firmware_backend_absent` record for the subcase)
   and emit two records from its report:
   `case=firmware_overrun` (timeout result, timeouts count, `disabled`,
   later-call result after quarantine, survivor progress) and
   `case=disallowed_io` (policy and denied results, `disallowed` count,
   `pic_unchanged`, `pit_unchanged`, `mappings_ok`, PIC masks and PIT status
   before/after). Keep each record ≤ 240 bytes. The subcases MUST NOT run
   the real firmware (the self-test's synthetic mode) and MUST leave the
   virtual PIC/PIT and the physical controller untouched, as the self-test
   already guarantees; the survivor of the probe advances 100 ticks after
   them as for the other subcases.
2. Replace the `not_run_subcases` declarations of the `firmware_overrun`
   and `disallowed_io` cases (and the desktop-profile siblings) with
   predicates on the new records: `timeout_result=-110`, `timeouts=1`,
   `disabled=1`, `later_result=-5`, `policy_result=0`, `denied_result=-1`
   (or the exact `V86_E*` codes the probe prints), `disallowed=1`,
   `pic_unchanged=1`, `pit_unchanged=1`, `mappings_ok=1`, `survivor_progress ≥ 1`.
3. Host tests: the i8042/biosvm harness runs the new subcases with the
   fake firmware and checks the records; predicate tests with records
   produced by the probe's own formatting.

## Acceptance by the lead

Host tests; kernel build; on QEMU the `f1-input` suite ends with the two
cases PASS on every profile and the `qemu-desktop-*` cases reached and
passing. Reply with: files, the record table, the host test output.
