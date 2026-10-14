# f1-60 — Lexar stick on the T23: the first READ(10) gets 448 of 512 bytes and times out; a failed volume must not be published

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-60` from `main`. Host-testable; the lead runs `f1-usb` on QEMU and the T23.

## Evidence (T23, image ce2e6ce9, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/ce2e6ce9/disk-log/BOOT.LOG.clean`, lines 710-735)

f1-59 works: no freeze, and the bind is recorded step by step: `stage=max_lun`, `interface=0 addr=2 max_lun=0`, `stage=inquiry`, `stage=ready`, `stage=capacity`, `stage=flush`, `stage=mode`, `ready addr=2 sectors=31283200 generation=32856 mode=rw_capable` (16 GB, correct), `stage=mount`, then:

```
[usb-storage] fault addr=2 reason=data_transfer tag=175 cmd=28 stage=data expected=512 actual=448 result=-110 td=1800003f toggle=1 csw=0 status=0 residue=0 port=00000095
[usb-storage] td addr=2 tag=175 valid=1 ep=1 pid=69 toggle=1 expected=64 actual=64 td=1800003f token=07e88269 hc_status=00000002 result=64
[usb-storage] timing addr=2 tag=175 frames=3 elapsed_ms=3 retired=0
[usb-storage] csw addr=2 tag=175 available=0 signature=00000000 csw_tag=0 status=0 residue=0
[usb-storage] quarantine addr=2 reason=data_transfer stage=mount transfer=data
[usb-storage] reset addr=2 result=-110
[usb-storage] volume drive=E part=1 fat=32 mode=rw dirty=0 error=-5
[usb-storage] mount addr=2 result=-11
[usb-storage] attach addr=2 result=0
[usb-storage] detached addr=2 reason=bind_deadline
```

The first sector read of the mount (READ(10) of 512 bytes at full speed = eight 64-byte bulk IN packets) received seven packets (448 bytes) and the eighth never retired within the stage budget (-110), although the traced TD shows a complete 64-byte IN (`actual=64 td=1800003f`, inactive, no error, toggle 1). The INQUIRY (36 bytes, one packet) and READ CAPACITY (8 bytes) worked. Then the volume was still given the letter E with `error=-5` and `mount result=-11`, so the desktop and the DOS VM disk probe saw a drive that returns I/O errors (f1-61 handles the probe). e86305de's `usb-storage-fault` on the same stick is the same symptom.

## References

UHCI Design Guide 1.1 (TD chains, data toggle per TD, SPD/short packet, the `element` pointer advance), USB 2.0 §5.8 (bulk, toggle sequence, max packet 64), USB Mass Storage BOT 1.0 §5-6, Linux `drivers/usb/host/uhci-q.c` (`uhci_submit_common`: one TD per max-packet, toggles alternating per TD, `td->status` SPD on the last TD), `drivers/usb/storage/transport.c`.

## Task

1. Reproduce on the host UHCI model a 512-byte bulk IN split into eight 64-byte TDs where the device answers every packet, with toggles alternating 0,1,0,1,...; the eighth TD must retire. Audit the transfer loop (`usb_storage.c:38-50`, the per-TD submission in `uhci.c packet()`/f1-56 walker, f1-57 NAK handling, f1-59 stage budget): toggle bookkeeping across packets (`toggle=1` on the eighth = wrong if the seventh was 1), `element` pointer/QH handling after the seventh retirement, a TD list of only seven entries, SPD handling, the 1 s stage budget being charged before the last packet. Fix the cause; the host model must fail before and pass after.
2. Do not publish a volume whose mount failed: no drive letter, `[usb-storage] volume ... error=` stays, the device quarantined with the stage; Files and the DOS VM must not see E:.
3. Keep a record of the toggle and TD index per packet in the fault trace (first eight entries) so the next T23 log is conclusive.

## Acceptance

`tests/host/usb/run.sh`, `storage_run.sh`, `test_runner_usb.py` green; the lead runs `f1-usb` on QEMU and the T23: stick mounted and readable in Files, or a fault trace naming the packet. Files: `src/kernel/usb/*`, `tests/host/usb/*`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first where named (`--search`), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f1-60-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
