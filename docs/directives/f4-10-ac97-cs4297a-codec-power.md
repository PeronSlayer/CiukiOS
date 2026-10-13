# f4-10 — AC'97 on the T23: the CS4297A never reports its analog sections ready

- **Model/effort:** `gpt-6-astra`, `xhigh`, `--search` (third attempt on this codec after f4-08 and f4-09 with `gpt-6.1-sol`; owner rule of 2026-10-11 satisfied). **Worktree:** `../CiukiOS-wt-f4-10` from `main`. Host-testable; the lead runs the T23 capture.

## Evidence (T23 ThinkPad, ICH3-M 8086:2485, image 24c98799, 2026-10-13)

```
[audio] controller=8086:2485 codec=43525936 ready=00300100 power=0000 master=0000 pcm=0000 rate=0 irq=11 error=-110 reason=stop_unproved
[audio] failure stage=codec_power ext2a=0000 rate2c=bb80 pisr=0001 posr=0001 mcsr=0001 pocr=00 civ=0 lvi=0 piv=0 picb=0
```

`ac97_codec_prepare` (`src/kernel/drivers/ac97.c`, stage `codec_power`) writes 0 to the PR bits of register 26h and waits for the ready bits (3:0) within the 500 ms activation deadline; the register reads 0000h for the whole wait. Registers 02h and 18h also read 0000h; 2Ch reads BB80h, 7Ch/7Eh give the id (CS4297A), GLOB_STA shows the primary codec ready bit (0100h) and no read-timeout bit. The ICH3 silent DMA proof of f4-09 never ran because the codec stage failed first. On QEMU (AC97 model) the same code works.

## Task

1. Research before changing: the Cirrus Logic CS4297A data sheet (register 26h ready bits and calibration time after reset, vendor registers 5Ah–7Ah, any power-management state the firmware may leave), the Intel 82801CAM (ICH3-M) data sheet AC'97 controller chapter (cold/warm reset, codec access semaphore, read-completion status, the "ACLINK shut off" bit, bit-clock requirements), and the Linux `sound/pci/intel8x0.c` and `sound/pci/ac97/ac97_codec.c` bring-up order (reset register 00h write, codec-ready wait, the analog-ready wait and what Linux does when it expires: it warns and continues). Cite the sections with URLs.
2. Decide and implement a bring-up that works on this codec: register 00h reset write where the references require it; a longer, bounded analog-ready wait (reference values); treat "analog not ready" as a warning that does not stop activation when the codec id and a write/read-back test (for example master volume 02h: write 8000h, read back) prove the codec responds; keep the f4-09 silent DMA proof as the gate for `ready`. Record every stage with the register values (`[audio] stage=… reg26=… reg02=…`) so the next capture is conclusive.
3. If the registers 02h/18h/26h reading 0000h means the codec is not answering (the controller returning stale zeros), find why from the ICH3 semantics (semaphore, read timeout, link off) and fix that first.
4. Host tests: a fake CS4297A that never raises the ready bits but answers reads and writes must now activate (after the DMA proof); a fake that returns 0 to everything must still fail with a precise reason; the QEMU-model fake keeps passing.

## Acceptance

`tests/host/audio/run.sh` green; i686 kernel compiles; report with the references and the final sequence. The lead qualifies on the T23 (desktop sounds, Music Player).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-10-report.md`.
