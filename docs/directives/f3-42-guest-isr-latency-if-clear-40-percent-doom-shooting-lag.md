# f3-42 — DOOM still lags when shooting: the guest sees the SB16 IRQ 11 ms late on average because the virtual IF is clear 40 % of the time and the VM is descheduled 12 %

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-42` from main (after `ffeca72b`).
- **Authority:** `src/kernel/vm/dosvm.c` (IRQ delivery, `dosvm_deliver_irq`, VIF tracking), `dosvm_dpmi*.c` (PM IRQ reflection, ISR trampoline return), `v86*.c` (CLI/STI/IRET shadows), `dosvm_sb16.c` (cadence counters of f3-40), `src/kernel/core/task.c` (f3-41 preference), host tests `tests/host/dosvm_*`, the DOOM `GUEST_PERF` replay.

## Evidence (T23, image 041b221f, `legacy/local/physical/041b221f/BOOT.LOG`, Normal boot, DOOM windowed then native full screen; read-only)

```
[vm-audio] vm=1 cadence blocks=431 late_acks=7  merged=62  ack_max_ms=501 irq_latency_max_us=504472 underruns=0 irq_latency_avg_us=2896  descheduled=150 parked=501 if_clear=216
[vm-audio] vm=1 cadence blocks=430 late_acks=0  merged=0   ack_max_ms=6   irq_latency_max_us=6279   underruns=0 irq_latency_avg_us=928   descheduled=202 parked=0   if_clear=3
[vm-audio] vm=1 cadence blocks=431 late_acks=49 merged=193 ack_max_ms=60  irq_latency_max_us=59602  underruns=0 irq_latency_avg_us=11354 descheduled=592 parked=0   if_clear=2014
```

- Owner: DOOM lags heavily when shooting or opening doors, windowed and full screen; sound distorts (the AC97 backend was dead in this boot, f0-39, so the physical output cannot be judged, but the guest-side cadence is measurable).
- With f3-40 the DMA no longer waits (`underruns=0`), yet in the busy window 193 of 431 blocks (45 %) merged their IRQ, the guest ISR ran 11.4 ms late on average and 60 ms at worst; of each 5 s the SB IRQ was pending with the virtual IF clear for 2014 ms and with the VM descheduled for 592 ms. Real DOOM keeps IF clear for microseconds; 40 % means the monitor's view of IF is wrong or the ISR exit is not observed promptly.
- Candidates: (a) in DPMI PM32 the ISR's IRET pops EFLAGS in ring 3 and the CPU ignores IF, so the monitor learns the ISR ended only through the reflection trampoline/exit path; a delayed or missed exit keeps `vif` clear; (b) CLI/STI shadows (`v->dpmi.shadow`, `monitor.shadow`) left set; (c) the IRQ 0 timer ISR and the SB ISR nesting with our per-delivery work; (d) f3-41's preference not applying while `if_clear`.

## Required

1. Instrument first (host DOOM replay with `GUEST_PERF`): per-vector ISR duration as seen by the monitor (delivery → observed return), time with `vif` clear outside any ISR, and the path that re-enables `vif` (trampoline exit, STI trap, IRET in V86, POPF). Report the distribution for DOOM's IRQ 0 and IRQ 7 with and without sound effects (`-nosound` vs sound on; the replay must include a sound-on run with the SB16 model active).
2. Fix what the measurement shows: the ISR end must be observed within one trap of the guest's IRET (if the trampoline is slow or missing for some ISR paths, add the cheapest exit: a HLT/INT in the trampoline or a single-step on the IRET frame only for hardware IRQ frames), shadows must clear at the right instruction, nested IRQ accounting must not keep `vif` clear after the outer IRET. The virtual IF must mirror the guest's EFLAGS.IF at every exit.
3. Target on the replay: average SB IRQ latency ≤ 2 ms, `if_clear` ≤ 10 % of the window with sound on, `merged` ≤ 5 % of blocks; `descheduled` reported so the lead can judge f3-41's preference.
4. Host suites: `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `bash tests/host/audio/run.sh`, `python3 scripts/build_kernel.py`; the DOOM replay pair (before/after) with the exact commands.
5. Report `build/codex/f3-42-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No physical PIC writes; no change to the native full-screen provider; keep the f3-40 SB16 model.
