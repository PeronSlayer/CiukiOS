# Directive f1-03: rebase f1-02 on main, F1 dispatch and probe registration

- **Step:** F1. **Contracts:** `f1-acceptance.md` (selection, evidence,
  runner), `f0-acceptance.md`, `boot-memory.md` (BOOT.CFG options).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-runner` (the f1-02 delivery, uncommitted), to be
  rebased by you onto current `main`, which now contains f0-01 (parser
  hardening, NOT_RUN, canonical selector reconstruction), f1-00 (kernel
  services) and f1-01 (filesystem code under `src/kernel/fs/`). The f1-02
  delivery is committed on the branch as `72d0ab6`; merge `main` into it
  (`git merge main`), resolve the conflicts and commit on this branch only. Files: those of f1-02 plus
  `src/kernel/include/ciuki/probe.h`, `src/kernel/linker.ld`,
  `src/kernel/probes/probes.c`, `src/boot/ciukldr/menu.inc`,
  `tests/host/kernel_lib_test.c` if a host check is useful. Nothing else.

## What to do

1. **Merge.** Bring `main` into this branch (`git merge main`), resolve
   every conflict and commit on this branch (never on `main`, never push).
   Keep every f0-01 behaviour:
   printable bytes only, ≤64 bytes, known names, `platform=e500` and
   `safe=1` accepted only with their boot flags, stop-on-failure with
   `NOT_RUN`, canonical request reconstruction in `run.py`, `loader_options`
   refusal, timer relations. Keep every f1-02 behaviour listed in its
   report. Host tests of both must pass (f0-01 added 6, f1-02 added 14).
2. **Dispatch out of the parser.** `parse_selector` returns only data:
   phase (0/1), probe name, run id, flags. All record emission and dispatch
   moves to `probes_main`. F0 names dispatch to the F0 table; F1 names to
   the F1 table (below); `all`/`core` apply within the selected phase
   (`f1:core` = every F1 probe but none of F0; for F1 nothing is destructive,
   so `core` and `all` coincide). A missing F1 probe yields
   `BEGIN`, `READY table=f1 installed=<n>`, `ERROR status=not_run
   reason=missing_probe`, as f1-02 did.
3. **F1 probe registration without a central table.** Add to `probe.h`:
   `struct probe_def { const char *name; int (*fn)(void); };` and
   `#define CIUKI_F1_PROBE(id, func) static const struct probe_def
   __attribute__((used, section(".f1probes"))) f1probe_##func = { id, func }`.
   In `linker.ld` collect `KEEP(*(.f1probes))` inside `.rodata` between
   `__f1probes_start` and `__f1probes_end`; `probes_main` iterates that
   range. Driver directives register their probes in their own files.
4. **Loader BOOT.CFG.** `safe=1junk` currently enables safe mode because the
   option is matched by prefix: options must be whole tokens separated by
   spaces or end of line; a malformed token is a selection error reported on
   screen and serial (`SELECT_ERROR` path), not silently ignored. Mirror the
   rule in `loader_model.py` with a test.
5. **No fixture transport into the guest.** `ciuki_boot_info` v1 is frozen
   and carries only the 64-byte selector, so the fw_cfg `fixture` item and
   the manifest `file_id/size/sha256` fields of f1-02 are removed. Instead
   the guest reports what it read (`lba=… sha256=…`, file names, sizes and
   SHA-256 digests) and the runner compares those records with digests it
   computes itself from the backing image or the overlay (`qemu-img`
   read-only export, `mtools`, `hashlib`). Update the f1 suites' predicates
   accordingly; the `ata-fault` subcases run inside one guest boot against
   a scripted fake behind the driver's register boundary (directive f1-06),
   so the suite has one `ata-fault` case plus one `ata-fault-blkdebug` case
   that injects a real read error through blkdebug and expects `EIO`,
   quarantine and zero further commands.
6. **Contract note for the lead** (no file change needed from you): the
   kernel keeps `f1:all`/`f1:core` as aliases for photographed hardware
   runs; the runner still expands aliases into ordered single-probe boots.

## Host tests (mandatory)

All existing runner and loader-model tests green (expect 50); add: F1
dispatch test on the production parser (names, phase separation, alias
scope), BOOT.CFG token rule in the model, and a static check that the
kernel map (`build/f0/VMM.map`) contains `.f1probes` between the two
symbols after `python3 scripts/build_kernel.py`.

## Acceptance by the lead

Diff review; host tests; kernel and loader build; on QEMU `f0-smoke`,
`f0-core`, `f0-panic` still pass and `f1-input` reaches `READY` and ends
`not_run` for every missing probe. Reply with: files, test output, and any
contract problem found.
