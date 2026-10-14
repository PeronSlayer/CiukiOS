# f1-62 — Lexar stick on the T23 (image 66843edc): the five-second bind budget is spent before the mount's first read

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f1-62` from `main`. Host-testable; the lead runs `f1-usb` on QEMU and the T23.

## Evidence (T23, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/66843edc/disk-log/BOOT.LOG.clean`, lines 523-545)

Enumeration at attempt 2 (`get_descriptor ... attempt=2 tick=34338 result=8`), then `stage=max_lun`, `interface=0 addr=2 max_lun=0`, `stage=inquiry`, `stage=ready`, `stage=capacity`, `stage=flush`, `stage=mode`, `ready addr=2 sectors=31283200 mode=rw_capable`, `stage=mount`, and the mount's first command fails before its first packet:

```
[usb-storage] fault addr=2 reason=invalid_csw tag=56 cmd=00 stage=csw expected=13 actual=0 result=-110 ...
[usb-storage] bulk addr=2 tag=56 packets=1 index=0 offset=0 stop=bind_deadline elapsed_ms=0 deadline_ms=39297 bind_left_ms=98
[usb-storage] packet addr=2 tag=56 index=0 toggle=0 expected=13 actual=-1 result=-110 ... valid=0
[usb-storage] quarantine addr=2 reason=invalid_csw stage=mount transfer=csw
[usb-storage] volume drive=E part=1 fat=32 mode=rw dirty=0 error=-5 / mount addr=2 result=-11
```

`bind_left_ms=98`: the stages between max_lun and mount (inquiry with retry, TEST UNIT READY up to three times, READ CAPACITY, SYNCHRONIZE CACHE, MODE SENSE) consumed about 4.9 s of the aggregate 5 s bind budget on this stick, so the first mount transfer was refused by the deadline (`valid=0`, nothing submitted). f1-60's per-packet renewal cannot help when the aggregate budget is gone. The stage records carry no timing, so which stage was slow is unknown. Also `[usb-storage] volume drive=E ... error=-5` still shows a letter in the record (f1-60 says it is no longer published: confirm Files does not list it).

## Task

1. Record the elapsed milliseconds of every bind stage (`stage=<name> addr=.. elapsed_ms=..` on completion) and the total at `ready`.
2. Budget: 3 s per stage, 20 s aggregate (USB 2.0 and BOT give sticks seconds to spin up; TEST UNIT READY may return "becoming ready" for a while), the mount's first reads get their own 3 s; the bind must still fail closed and quarantine on exhaustion, and other controllers' input must never wait for it (see f0-20).
3. Host regressions: a stick whose READY takes 4 s and FLUSH 2 s must mount; one that never becomes ready quarantines at 20 s with the stage named.

## Acceptance

USB host suites green; the lead runs `f1-usb`, then the T23 with the stick: mounted and readable in Files, or stage timings naming the slow step. Files: `src/kernel/usb/usb_storage.c`, tests. Not `usb_core.c`'s worker loop or the disk log (f0-20).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f1-62-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
