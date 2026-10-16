# f1-74 — USB stick: missing CSW during the mount read sequence, and a full-FAT scan that takes 19 s on USB 1.1

- **Phase:** F1. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f1-74` from main `bbac0713`.
- **Authority:** `src/kernel/usb/usb_storage.c`, `src/kernel/usb/uhci.c` (bulk path), `src/kernel/fs/mount.c`, `src/kernel/fs/fat.c` (mount/scan policy for removable volumes), `src/kernel/probes/hardware_check*.c` (USB item budget), host tests, `docs/design/usb-contract.md` and `docs/design/vfs-storage-contract.md` (amend).

## Evidence (T23, image e8dfd0cd, `legacy/local/physical/e8dfd0cd/serial.log`, read-only; stick 05dc:a81d, 31,283,200 sectors, FAT32 with 1,953,289 clusters of 16 sectors)

```
[usb-storage] ready addr=2 sectors=31283200 generation=2304 mode=rw_capable elapsed_ms=626
[usb-storage] stage=mount addr=2
[hwcheck] FAIL USB storage: enumerated=1 mounted=0 binding=1 quarantine=0 hosts=3 scanned=3 reason=no_volume ms=13002
[usb-storage] packet addr=2 tag=207 index=7 toggle=0 expected=64 actual=64 result=64 td=3900003f token=07e08269 frames=4 elapsed_ms=4 retired=0 valid=1
[usb-storage] csw addr=2 tag=207 available=0 signature=00000000 csw_tag=0 status=0 residue=0
[usb-storage] quarantine addr=2 reason=data_transfer stage=mount transfer=data
[usb-storage] reset addr=2 result=-110
[usb-storage] volume drive=E part=1 fat=32 mode=ro dirty=0 error=-5 mount_ms=19169 repair_ms=0 clusters=1953289 spc=16
[usb-storage] bind addr=2 elapsed_ms=19867 result=-5 published=0 overrun=0 reads=101 sectors=12293 max_sectors=128
[usb-storage] cadence addr=2 packets=98746 poll_frames=10019 retire_frames=9262 batch=32
```

Two defects. (a) The mount read sequence (101 reads, 12,293 sectors, about 6 MB of FAT) ends with a command whose CSW is never observed (`available=0 signature=0`), the recovery reset times out and the device is quarantined with EIO. This is the first hardware run with UHCI in interrupt mode (f1-71/f1-72): the bulk path's completion/retirement order and the CSW wait must be checked against the BOT specification and the f1-72 wake-ups (a completion notification arriving before the TD is retired, a toggle or short-packet handling after `retired=0 valid=1` packets). The second boot shows the same stop at `stage=mount`. (b) Even without the fault, the ownership scan of a 7.8 MB FAT over USB 1.1 (about 330 KB/s) takes more than the 13 s the hardware-check item allows and more than any user tolerates: standard FAT drivers mount removable media immediately and trust the dirty flag and FSInfo; the ownership scan is needed only before the first write.

## Required

1. Reproduce (a) on the host BOT/UHCI model in interrupt mode with the production usb_storage/uhci code: a 64 KiB read sequence of 128-sector commands, completions delivered through the f1-72 notification path, and find why the CSW stage reports no data; fix the ordering (retire before CSW evaluation, toggles, short packets, the stall/clear-feature recovery) against USB Mass Storage Bulk-Only 1.0 and UHCI 1.1; a transfer that fails once must retry the command (BOT reset recovery) before quarantining the device.
2. Removable volumes mount read-only immediately without the full ownership scan (FSInfo and dirty flag only, bounded reads), publish the drive letter at once, and run the ownership scan only on the first writable upgrade (f0-25 path) or in the background with the bounded budget of f1-70; the hardware-check USB item reports the mount time and the scan state separately. Fixed system volumes keep the current policy.
3. Host coverage for both; run `tests/host/usb/run.sh`, `tests/host/usb/storage_run.sh`, `scripts/test/host_fs_tests.sh`, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f1-74-report.md`. The lead runs f1-usb, f1-storage, f4-media (safe-remove-usb) on QEMU and the H page on the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
