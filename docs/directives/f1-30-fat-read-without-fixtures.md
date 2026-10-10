# Directive f1-30: `fat-read` without fixture disks must report `not_run`, not `FAIL`

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`fat-read` row:
  FAT12/16/32 fixture volumes supplied by the runner; NOT_RUN rule for
  missing prerequisites), `test-architecture.md` (fixtures exist only on
  QEMU overlays; the hardware tier has the boot disk only).
- **Implementer:** Codex, `gpt-6-luna`, effort `medium`.
- **Worktree:** `wt/f1-fat-read-hw`, from `main` at or after the f1-29
  merge. Files: `src/kernel/probes/fat_probes.c` (`probe_fat_read`
  verdict only), `tests/host/fs/test_storage.c`, `tests/suites/f1-fat32.json`
  (the three `fat*-read` cases only if a record changes),
  `scripts/test/physical.py` (import classification of the new record).

## Observed on the ThinkPad T23 (image `cb33aec3…`, sweep `44444444`, 2026-10-11)

```
probe=fat-read … case=fixture disk=1 status=absent
probe=fat-read … case=fixture disk=2 status=absent
probe=fat-read … case=fixture disk=3 status=absent
probe=fat-read … case=mount drive=C disk=0 type=32 mode=ro reasons=1 writes=0 read_gate=1
probe=fat-read … case=invalid_name invalid=-22 unmappable=-84 writes=0
probe=fat-read … group=metadata subcase=complete owner=vfs generation=1 errors=1 gate=failed timing_domain=hardware
probe=fat-read event=END status=FAIL error=-5
```

With no fixture disk attached the probe has nothing to read and ends
`FAIL error=-5`; on hardware there is never a fixture disk, so the
sweep records a failure that is not one. The boot volume's own read
checks (`mount`, `invalid_name`) passed.

## What to do

1. When no fixture disk is present, the probe emits
   `event=ERROR status=not_run reason=fixtures_absent` after the boot
   volume checks and ends `END status=NOT_RUN` (or the equivalent the
   records grammar already uses for operator-gated cases), never
   `FAIL`; with fixtures present nothing changes.
2. The physical import classifies that outcome as `not_run` for the
   three `fat*-read` cases with the reason, so the hardware summary
   shows what the tier cannot cover.
3. Host test: storage harness without fixture volumes → the records
   above and the not_run verdict; with fixtures → unchanged PASS.

## Acceptance by the lead

Host tests; QEMU `f1-fat32` read cases unchanged; the next T23 sweep
shows `fat-read` as `not_run reason=fixtures_absent`. Reply with: files,
the record, test output.
