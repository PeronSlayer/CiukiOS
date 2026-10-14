# f4-15 — AC'97 on the T23: activation succeeds, playback fails after about 1.4 s

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f4-15` from `main`. Host-testable; the lead re-tests on the T23 once f4-14 restores clean captures.

## Evidence (T23, image 293c05af, 2026-10-14; the serial capture is garbled by the f4-13 queue, fragments in `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/serial-clean.log`, disk log tail in `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/disk-log/BOOT.LOG`)

Activation completes (`[audio] controller=8086:2485 … power=000f master=0404 pcm=0808 … result=ok`, the boot beep is heard), then during the first playback: `[audio] play=failed civ=8 … elapsed_ms=1377` and `[audio] backend f(ailure) … (reti)red=209 running=1 active=0`; afterwards no sound at all (UI sounds, DOOM, Wolf3D) because the backend stays failed. The same pattern was seen in e86305de (`[audio] progress err=-5`) and f4-13 added the zero/reloaded PICB tolerance, which did not prevent this failure: `civ=8` with an 8-descriptor active window suggests the hardware's current index ran past the published window (underrun / CIV wrap handling), or the cursor comparison still rejects a legitimate wrap.

## Task

1. Model the ICH3 bus master precisely (82801CAM datasheet, chapter 13: CIV/LVI/PICB semantics, the 32-entry BDL ring, LVI reached → DCH halt, CELV/LVBCI/BCIS bits) and reproduce on the host the state `civ=8` with `retired=209` and the published window of eight: which comparison in `ac97.c`/`backend.c` fails and why (wrap of CIV modulo 32 vs the published counter, LVI not advanced in time because the mixer thread was late, the serial stall that f4-13 queued but that still blocks the timer?).
2. Fix: the ring must survive late refills (LVI kept ahead, silence inserted on underrun instead of failing, counters resynchronised from CIV/PICB after a halt) and failures must carry the full snapshot in one record (`civ lvi piv picb sr published retired running`) so the next capture is conclusive; the T23 must keep playing across the whole session.
3. Host tests: underrun with late refill, CIV wrap, DCH recovery; the QEMU model unchanged.

## Acceptance

`tests/host/audio/run.sh` green; i686 kernel compiles; the lead hears continuous audio on the T23.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-15-report.md`.
