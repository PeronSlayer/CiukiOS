# f4-09 — AC'97 on the T23: the bring-up fails with `error=-110 reason=stop_unproved` (Cirrus Logic CS4297A)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` (Intel ICH3-M 82801CAM AC'97 controller datasheet, Cirrus Logic CS4297A datasheet). **Worktree:** `../CiukiOS-wt-f4-09` from `main`. Host-testable (register model); the lead qualifies on the T23 with an `L` capture.

## Evidence (T23, 2026-10-13, image ceb40b69, verbose boot)

```
[audio] controller=8086:2485 codec=43525936 ready=00300100 power=0000 master=0000 pcm=0000 rate=0 irq=11 error=-110 reason=stop_unproved
[init] audio init done
```

Controller Intel ICH3-M (8086:2485), codec id 43525936 = "CRY6", Cirrus Logic CS4297A. The f4-08 bring-up reached the codec (ready and power registers read, master and PCM unmuted at 0 dB), but the transport proof failed: `stop_unproved` after a timeout (-110 = ETIMEDOUT) at `src/kernel/drivers/ac97.c` ~458, and `rate=0` suggests the sample-rate step did not take (VRA not supported or not enabled: the CS4297A supports VRA in reg 2Ah; otherwise 48 kHz fixed). The audio device is quarantined, so no boot tone, no system sounds, no DOS game sound on the T23. On QEMU (emulated codec) every step passes.

## Task

1. Reproduce from the register model the ICH3 + CS4297A behaviour: the stop proof (`stop_unproved`): what the driver waits for (DCH bit of the bus-master status after clearing RPBM? the CIV/LVI equality? a DMA halted interrupt?) and what the ICH3 actually does when a channel is stopped with no buffer descriptors programmed or with a 0 kHz rate. Fix the sequence per the datasheets: reset the channel (RR bit), program a valid BDL even for the proof, set a valid rate (48000 when VRA is absent; enable VRA in 2Ah bit 0 before writing 2Ch and verify by reading back), then start/stop with bounded waits; treat "rate readback differs" as fixed 48 kHz, not as a failure.
2. Keep the boot tone in `L` mode and the `[audio]` records; add the raw values of 2Ah/2Ch and the bus-master status words to the record on failure.
3. Host tests: a CS4297A-like codec model without VRA, with VRA, with slow ready; an ICH3 bus-master model for the stop proof; the `f4-audio` harness stays green.

## Constraints

No QEMU, no commits, no diary; `nice -n 19`; `rg` not semble; one host suite at a time. Report `build/codex/f4-09-report.md` with the datasheet references and the register sequence.
