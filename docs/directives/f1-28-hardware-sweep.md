# Directive f1-28: unattended hardware sweep — selector from `BOOT.CFG`, sweep aliases, self-reboot

- **Step:** F1 (test infrastructure for the T4 hardware tier, used by F0, F1 and F2).
  **Owner request (2026-10-11):** the hardware qualification must not require
  typing a selector at the loader for every probe; one disk write, one
  serial capture, one power-on must run the whole F0→F2 matrix.
- **Contracts to amend:** `test-architecture.md` (hardware tier: one
  capture per sweep, selector sources), `f1-acceptance.md` (selector
  grammar sources: menu, fw_cfg, `BOOT.CFG`; `safe-boot-cfg` already
  reads `\SYSTEM\BOOT.CFG`), `boot-memory.md` (BOOT.CFG contents),
  `f2-acceptance.md` (an `f2:sweep` alias beside the per-case suites, with
  the explicit statement that QEMU suites keep one boot per case and the
  sweep is hardware evidence), `execution-abi.md`/`posix-subset.md` are not
  touched.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f1-hardware-sweep`, from `main` at or after `6b7e597`.
  Files: `src/boot/ciukldr/menu.inc`, `disk.inc` (reading the selector
  line), `src/kernel/probes/selector.c`, `probes.c` (sweep aliases, step
  cursor), a new `src/kernel/core/reboot.c` (+ header; `arch/` only for a
  triple-fault/reset stub), `src/kernel/fs/mount.c` only for a bounded
  helper that rewrites `\SYSTEM\BOOT.CFG`, `scripts/test/run.py` and
  `scripts/test/physical.py` (multi-boot capture import), `tests/host/*`
  (loader model, selector, sweep order, reboot stub, import),
  `tests/host/record_scope_test.py`, `docs/build-and-run.md` (hardware
  procedure), the contract documents named above. The T1 loader-size
  check must still pass.

## Observed on the ThinkPad T23 (2026-10-11)

The owner ran `f0:all` and `f1:all` by pressing `P` at the loader menu
within 3 s and typing the selector on the laptop; `f1:all` stopped at the
first failing probe (`input`, directive f1-27) and marked the rest not
run; F2 has no `all` alias; multi-boot probes (`fat-write` cold reboot,
`bootlog` reopen, `mount-crash` cut and reboot) rely on the runner to
reboot the VM on QEMU and on a human on hardware. Nine F2 probes plus the
remaining F1 probes would need about twenty manual boots with a fresh
capture each.

## Required behaviour

1. **Selector from the boot volume.** `\SYSTEM\BOOT.CFG` may carry one
   line `probe=<selector>` in the existing grammar (`f[012]:<probe|all|sweep>
   run=<8hex>`, no fw_cfg-only suffixes). The loader treats it as a
   selector source of the same rank as the menu `P` entry: when present
   it skips the 3 s prompt and boots the request; the menu keys still
   work during the countdown to override it (`N` normal, `S` safe). Record
   the source in the loader marker (`L:SELECT_SOURCE=cfg|menu|fw_cfg`).
2. **Sweep aliases.** `f0:sweep`, `f1:sweep`, `f2:sweep` run every probe
   of that table in the table order with **one run id for the whole
   sweep**; a failing probe does not stop the sweep (each probe keeps its
   own BEGIN/END; the alias emits `event=SWEEP step=<n> probe=<name>
   result=<pass|fail|not_run>` after each and `event=SWEEP_END
   passed=<n> failed=<n> not_run=<n>` at the end). `f1:sweep` places the
   multi-boot probes last; `f0:sweep` places `panic` last.
3. **Continuation across reboots.** Before running a step the kernel
   writes the next step into `BOOT.CFG` (`probe=f1:sweep run=<id>
   step=<n+1>`, durable: write + barrier through the production storage
   path, with the write gate opened for that single file; on a read-only
   volume the sweep records `not_run reason=readonly` and halts). A probe
   that needs a reboot (its `ARM action=cold_reboot`/crash cut/`bootlog`
   reopen) is followed by a kernel self-reboot: 8042 pulse `0xFE` first,
   then a triple fault; the step cursor already points at the same probe's
   second boot. A hang or panic is recovered by the human power cycle:
   the loader reads the cursor and continues with the next step. When the
   last step completes, the kernel removes the `probe=` line and halts
   with `SWEEP_END`.
4. **Chaining the three phases:** `probe=all:sweep run=<id>` runs
   `f0:sweep`, then `f1:sweep`, then `f2:sweep` with the same cursor file.
5. **QEMU unchanged.** The runner suites keep one boot per case; the
   sweep aliases are accepted by the selector on QEMU only through the
   same `BOOT.CFG` path (an overlay patch), used by one new runner case
   `sweep-smoke` that runs `probe=f0:sweep` end to end under `-no-reboot`
   handling (QEMU turns the self-reboot into a shutdown; the runner
   relaunches the overlay until `SWEEP_END`, bounded).
6. **Import.** `run.py <suite> --physical-capture <dir>` accepts a sweep
   capture containing many boots: split on the loader banner, attribute
   each probe's records to the suite case with the same probe name and
   profile `physical`, keep the per-case evidence rules (prerequisites,
   operator-confirmation cases), and write one summary.

## Host tests

Loader model: `BOOT.CFG` selector parsing, precedence with menu keys,
rejection of fw_cfg-only suffixes, size limit. Kernel: sweep order per
table, cursor write/read round trip on the FAT host harness, behaviour on
a read-only volume, the reboot stub call order. Runner: multi-boot
capture import on a synthetic capture with three boots and one panic.

## Acceptance by the lead

Host tests; kernel build; T1 checks; on QEMU `f0-smoke`, `f1-safe`
(`safe-boot-cfg` unchanged) and the new `sweep-smoke` case PASS; on the
T23 one power-on with `probe=all:sweep run=<id>` in `BOOT.CFG` runs the
matrix to `SWEEP_END` with the lead's import producing per-case results.
Reply with: files, the `BOOT.CFG` grammar line, the sweep tables, the
reboot sequence, test output, any contract problem found.
