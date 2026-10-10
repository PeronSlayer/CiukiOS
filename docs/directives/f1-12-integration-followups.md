# Directive f1-12: integration follow-ups after f1-09, f1-10, f1-11 and f2-05

- **Step:** F1/F2 integration. **Contracts:** `f1-acceptance.md` (boot log
  `\SYSTEM\LOGS\BOOT.LOG`, 128 KiB; input queue overflow policy: clear
  stale events, one RESYNC with the loss count and the button bitmap;
  boot identity amendment of 2026-10-11), `execution-abi.md` F2 extension
  (input events incl. `value 2 = repeat`, surfaces and mappings), the
  reports of f1-09 (`tests/host/fs/STORAGE-REPORT.md`), f2-05
  (`build/f2-05/REPORT.md` of its worktree, summarised in commit `67e7912`)
  and the patches the lead applies at merge.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f1-integration`, created from `main` after the lead has
  merged f1-10, f1-09 and f2-05 and applied the f1-09 output hook and the
  f2-05 `integration.patch`. Files: `src/kernel/core/bootlog.c`,
  `src/kernel/include/ciuki/bootlog.h`, `src/kernel/fs/mount.c`,
  `scripts/build_kernel.py` (probe object order, payload hooks),
  `src/kernel/drivers/i8042.c`, `src/kernel/include/ciuki/input.h`,
  `src/kernel/proc/grants.c` (input bridge simplification once the producer
  policy is right), `src/kernel/core/init.c`, `src/kernel/proc/supervisor.c`,
  `src/kernel/probes/*.c` only for record fields the aligned suites expect,
  `tests/host/*`, `scripts/test/host_kernel_tests.sh`, `tests/suites/f1-*.json`
  if a record name changes. Nothing else.

## What to do

1. **Boot log per contract**: path `\SYSTEM\LOGS\BOOT.LOG`, 128 KiB ring by
   truncation; the image builder already creates `/system`; `mount.c`
   creates `LOGS` after the write gate if absent. Update the bootlog probe
   fields accordingly.
2. **Probe object order**: `build_kernel.py` orders the linked F1 probe
   objects as the contract table: registry, input, input-fault,
   framebuffer, ata, ata-fault, partition, fat-read, fat-write, cache,
   mount-crash, safe, bootlog (`probes_fat_probes` after ATA and before
   safe). Verify with the map check.
3. **Input queue producer policy** (`i8042.c`, `input.h`): on overflow clear
   the stale queued events, enqueue one `RESYNC` event carrying the
   cumulative loss count and the current button bitmap, then accept new
   transitions; typematic repeats are emitted as key events with
   `value = 2` (not suppressed); the `input` probe counts transitions
   excluding repeats; the f2-05 input bridge then forwards events without
   consumer-side recovery (simplify `grants.c` accordingly). Host tests for
   both policies.
4. **Supervisor hooks already applied by the lead** (from the f2-05 patch):
   verify `supervisor_poll` in the PID 1 loop, `supervisor_bootstrap` after
   ordinary-boot driver activation, and the `CIUKI_DESKTOP_PAYLOAD_BIN`
   generation; fix anything the merge left inconsistent; the
   `crash-isolation` probe must no longer report `missing_standin_payload`.
5. **Storage identity record**: keep `qualified=0 reason=loader_fingerprints_absent`
   on the mount record (the amended contract accepts the disk-0/partition-1
   binding for F1 until boot-info v2 carries loader fingerprints).
6. Run everything: `tests/host` unittest (all green), kernel host suites,
   fs host suite, loader assembly, kernel build; report the final probe
   table order from the map.

## Acceptance by the lead

Host suites; kernel build; on QEMU the F0 suites, then `f1-input`,
`f1-safe`, `f1-storage`, `f1-fat32` on `qemu-t23` and `qemu-e500`, then
`f2-process`, `f2-runtime`, `f2-desktop` with the stand-in server. Reply
with: files, test output, the probe order, and any contract problem found.
