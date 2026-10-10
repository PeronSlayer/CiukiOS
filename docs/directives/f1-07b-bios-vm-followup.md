# Directive f1-07b: BIOS VM follow-up — merge, observed scan bytes, firmware workspace

- **Step:** F1. **Contracts:** `f1-acceptance.md` (firmware-first input:
  observed trapped reads supply transitions, amended 2026-10-11),
  `device-firmware-ownership.md` (firmware memory extents and VM-internal
  INT reflection, amended 2026-10-11), the f1-07 directive and its report
  (`build/f1-07/report.md` in the worktree).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`.
- **Worktree:** `wt/f1-bios-vm` (f1-07 delivery committed as `b3a10af`).
  The lead has run `git merge main` there: conflict markers remain in
  `src/kernel/core/task.c` (2 hunks: `schedule()` must keep both the f2-02
  `proc_task_switch` hook and the f1-07 `biosvm_task_cr3`/ESP0 hooks, and
  the task-exit hooks of both), `scripts/build_kernel.py` (source
  directories: keep `vm`, `proc`, `drivers`, `fs` and the f2-02 NASM
  constant extraction) and `scripts/test/host_kernel_tests.sh` (keep every
  test block). Files: those of f1-07 plus the three conflicted files; no
  git commands that write.

## What to do

1. Resolve the three conflicts; rebuild and rerun all host tests.
2. **Observed scan bytes**: when the firmware ISR reads port `60h` through
   the trapped `IN`, record the byte together with the AUX bit of the last
   `64h` status read into the existing raw set-1 decoder; `fwinput_poll`
   then yields key make/break transitions (and the mouse packets already
   captured by the callback) in addition to INT16 text. The firmware stays
   the only reader; no kernel read of `60h` is added. Count observed bytes,
   decoder resyncs and the agreement between INT16 text and observed makes.
3. **Firmware workspace**: map `C0000h–EFFFFh` read-write inside the BIOS VM
   (physical identity), `F0000h–FFFFFh` read-only, IVT/BDA/EBDA read-write,
   scratch/stack as today; record the mapping in the self-check report.
4. **VM-internal interrupts**: `INT n` executed by code inside the VM
   reflects through the IVT regardless of the kernel allowlist (bounded by
   the call deadline); the allowlist applies to kernel-initiated calls and
   the port policy stays as is.
5. **Verification against rel-1.16.3**: the pinned sources are at
   `/home/peronslayer/Desktop/CiukiOS/build/downloads/seabios/`
   (`mouse.c`, `bda.h`, `kbd.c`, `ps2port.c`, `Kconfig`, read-only): check
   the C2 callback stack layout, the BDA/EBDA fields and the `__process_key`
   behaviour your implementation relies on; fix and note any difference.
6. Keep `biosvm_set_rtc_cache` as the RTC boundary (provider arrives with
   f1-09); keep the quarantine rules of f1-07.

## Host tests (mandatory)

Extend `tests/host/v86_test.c`: observed-byte capture through the trapped
IN path (make/break sequences incl. E0, AUX separation), workspace mapping
table, internal INT reflection past the allowlist, and the merged
scheduler hooks (fake `schedule` with both proc and biosvm hooks).

## Acceptance by the lead

Diff review; host tests; kernel build with audit; on QEMU `qemu-e500`
(firmware-first): setup succeeds, QMP keystrokes produce text and make/
break transitions, mouse packets decode, self-check passes; F0 suites
unchanged. Reply with: files, test output, the verified SeaBIOS details,
and any contract problem found.
