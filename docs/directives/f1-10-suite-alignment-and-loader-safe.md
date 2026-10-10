# Directive f1-10: align the F1 suites with the probe records, loader safe monotonicity, console lock

- **Step:** F1. **Contracts:** `f1-acceptance.md` (probe table and evidence
  fields; safe-mode sources are monotonic: a positive source is never
  cleared by a later neutral one), `boot-memory.md` (BOOT.CFG and menu),
  the f1-08 report (`build/host/f1-08-report.txt` of the f1-08 worktree is
  summarised in the merge commit `eebc960`: probe record names and groups).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-suites`. Files: `tests/suites/f1-*.json`,
  `tests/host/test_runner.py` (predicate tests), `scripts/test/evidence.py`
  only if a predicate feature is missing, `src/boot/ciukldr/menu.inc`,
  `scripts/test/loader_model.py` and `tests/host/test_loader_model.py`,
  `src/kernel/core/console.c` and `src/kernel/drivers/fbdev.c` (shared
  presenter/console lock only), `src/kernel/include/ciuki/fbdev.h`.
  Nothing else.

## What to do

1. **Suite alignment**: read the records each F1 probe actually emits
   (`registry_probe.c`, `safe_probe.c`, `i8042_probe.c` for `input` and
   `input-fault`, `fbdev_probe.c`, `ata_probe.c`, and later `fat_probes.c`
   if present on `main`) and make the `f1-input`, `f1-safe`, `f1-storage`
   and `f1-fat32` suites' `where`/`fields`/`relations` predicates match
   them exactly, keeping every contract pass condition (counts, totals,
   zero-loss, quarantine counters, `optional_activations=0`, etc.). Where
   a probe does not emit a record the contract requires (for example the
   firmware overrun / disallowed-I/O evidence of `input-fault`, which
   comes from `biosvm_selftest` on the firmware-first profile), the suite
   case must expect the record the kernel does emit on that profile and
   `not_run` subcases must be visible, never silently passed. Case names
   use the probe's own subcase spelling.
2. **Loader safe monotonicity**: in `menu.inc` the Normal menu choice
   (`.normal`) clears `CBI_F_SAFE_MODE`; the contract requires that a safe
   mode already requested by `BOOT.CFG` or a validated fw_cfg selector is
   kept. Normal must only decline to *add* safe mode; `S` adds it. Mirror the
   rule in `loader_model.py` with a test.
3. **Console/presenter lock**: the kernel console and `fbdev_present/fill`
   write the same LFB. Add one sleeping-mutex-free rule compatible with the
   console's use from interrupt and panic paths: the console takes a short
   `irq_save` section per line as today; the presenter checks a console
   "busy" flag and the console marks the rows it owns (the text area) so a
   present never overlaps the console rows while the console is active,
   and the console skips drawing while a present of its rows is in
   progress (`fbdev` exposes `fbdev_console_region(rows)`); document the
   rule in `fbdev.h`. No change to the panic path's lock-free behaviour.

## Host tests (mandatory)

Suite predicate tests with fabricated record sets built from the probes'
actual record formats (one per F1 probe), the loader-model safe
monotonicity cases (BOOT.CFG safe + Normal, fw_cfg safe + Normal, menu S),
and a console/presenter region arbitration test with the heap LFB fixture.

## Acceptance by the lead

Host tests; loader assembly; kernel build; on QEMU `f1-input` and
`f1-safe` pass on `qemu-t23` and `qemu-e500` with the drivers started by
f1-08; `f0-core` unchanged. Reply with: files, test output, the per-probe
record tables you matched, and any contract problem found.
