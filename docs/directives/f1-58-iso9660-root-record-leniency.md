# f1-58 — The owner's CD is rejected as `malformed_iso9660` (root directory record `data_length`)

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f1-58` from `main`. Host-testable; the lead runs `f4-media`/`f1-storage` on QEMU and the T23 test.

## Evidence (T23, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean`, lines 33-47)

`[optical] bind slot=2 channel=1 unit=0 identified=1 type=5 packet_bytes=12 dma=0 mode=pio sector_size=2048`; media stages inquiry/capacity/toc/session/ready all `result=0`, `sectors=286613`; `iso session=0 lba=16 descriptor=pvd type=1 field=header result=0`; `field=volume_space_size declared=286611 readable=286611 result=0`; **`field=data_length result=-5`**; `field=root_directory_record result=-5`; `bind slot=2 stage=iso ... result=-5 reason=malformed_iso9660`; `[storage] optical slot=2 drive=- ... reason=malformed_iso9660`. The disc never appears. `src/kernel/fs/iso9660.c:70-83` (`record_named`) requires for the root record at PVD+156 that `both32(p+10)` holds (little-endian and big-endian copies equal), that a directory's `data_length` is ≥ 2048, a multiple of 2048 and ≤ `available*2048`; `:249-252` additionally `length==34`, `name_length==1`, identifier 0, directory flag. f1-54 added leniency elsewhere but not here. We do not know which disc the owner inserted (a 560 MB data CD); the next log must be conclusive by itself.

## References

ECMA-119 (ISO 9660) §8.4 (primary volume descriptor), §9.1 (directory record: both-byte-order fields, `data_length` 733 format); Linux `fs/isofs/inode.c` and `isofs/dir.c` (`isonum_733` reads the little-endian copy only; `isofs_readdir` iterates while `offset < i_size` by blocks, so unaligned directory sizes are read); Joliet SVD handling in the same files; `xorriso`/`mkisofs` behaviour notes on both-endian mismatches produced by old mastering tools.

## Task

1. `isofs` semantics for both-endian fields: use the little-endian value, record a mismatch (`field=... le=... be=...`) instead of failing, unless the LE value is itself impossible (zero directory, extent outside the readable volume: keep those failures). Directory `data_length` not multiple of 2048: round up for reading, never fail. Apply to the root record and to every directory record parsed later, and to the SVD (Joliet) path.
2. Every failing check logs the raw values it compared (`le`, `be`, `min`, `max`, `align`, `available`) at `klog` level, once per mount attempt.
3. Keep the bounds that protect reads (no extent past the readable end).
4. Host tests: Python-generated fixture images (no xorriso dependency): (a) LE/BE mismatch in the root `data_length`, (b) root directory of 2048+512 bytes, (c) root record with an extended-attribute length, (d) a Joliet SVD with the same quirks; each must mount and list files.

## Acceptance

`tests/host/test_optical.py`, `tests/host/fs` and `test_mount_fixtures.py` green; the lead's QEMU media suites; the next T23 test shows the disc bound (`storage optical ... result=0`) and visible in Files, or a conclusive record naming the exact field and values.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f1-58-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
