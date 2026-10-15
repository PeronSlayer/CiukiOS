# f3-23 — DOOM on the T23: interrupt delivery stops with IRQ 0 and IRQ 7 in service (image a738204a)

- **Model/effort:** `gpt-6.1-sol`, `xhigh` (Astra only if Sol fails twice). **Worktree:** `../CiukiOS-wt-f3-23` from `main`.

## Evidence (disk log, DOOM session pid 5, `window=DOOM`, VM generation 404970)

- `[vm-audio] vm=1 … base=220 irq=7 dma=1,5 dsp=4.05`; repeated `[vm-audio] vm=1 sb state=irq_wait reason=complete cmd=c6 dma=1 mask=0 mode=19 hz=11025`: the Sound Blaster completion interrupt (IRQ 7) is pending.
- `[dosvm] event=pic_stall vm=1 gen=404970 pic=0 irq=7 ms=1002 cs=01df eip=0024cc15 isr=81 irr=83 imr=78 vec=08 to=00ff:00000020 boundary=ret_step at=01df:0024cc10 pm=1 depth=0 ae=0 sm=0`
- `[dosvm] event=pic_stall vm=1 gen=404970 pic=0 irq=0 ms=1001 cs=01df eip=002529ac isr=81 irr=83 imr=78 vec=08 to=00ff:00000020 boundary=ret_limit at=01df:0025217e pm=1 depth=0 ae=0 sm=0`
- Afterwards `group=vm_perf_irq vm=1 n=331 first=0 counts=0,0,0,0,0,0,0,0` and `first=8 counts=0,…` for every later second; `vm_perf_rate frames_mfps=0`; `vm_perf_share guest_permille=520–559 busy_permille=741–767`: the guest spins at full speed and no interrupt is delivered any more. The owner saw DOOM "loading forever".

Both IRQ 0 and IRQ 7 are in service (`isr=81`), both still requested (`irr=83`), the timer vector points at the protected-mode handler (`00ff:00000020`), and the protected-mode delivery tracker stopped at a `ret_step` / `ret_limit` boundary.

## Task

1. Reconstruct the sequence from the virtual PIC and the protected-mode IRQ delivery code (f3-15, f3-19; `src/kernel/vm/dosvm.c` and the PIC/PM IRQ sources): how IRQ 7 (SB16 DMA completion) and IRQ 0 (timer) came to be in service together, why no EOI cleared them (non-specific EOI semantics, specific EOI, auto-EOI, the guest's handler chain through DOS/4GW), and what `ret_step` and `ret_limit` do when the handler's return is not observed.
2. Host replay from the records (`tests/host/dosvm_test.c` and the PIC model tests): the test must reach the same `isr=81 irr=83` state before the fix and deliver the pending timer after it.
3. Fix: delivery must resume (correct ISR/EOI handling for nested PM handlers, or the tracker boundary must not freeze delivery). No change to the guest-visible 8259 behaviour beyond the specification.
4. The SB16 `irq_wait` state machine: a completion interrupt that stays undelivered for more than 500 ms must be visible in one record with the ISR/IRR state.

## Acceptance

Host suites green with the new case; QEMU `f3-vm` `doom` and `doom-sound` cases green; the next T23 log shows `vm_perf_irq` counts above zero through the whole DOOM session and no `pic_stall`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f3-23-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
