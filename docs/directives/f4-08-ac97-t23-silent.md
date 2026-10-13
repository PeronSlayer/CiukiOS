# f4-08 — No sound on the T23: AC'97 bring-up on real hardware

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` (Intel ICH3-M AC'97 controller and AC'97 2.x codec specifications, Analog Devices/Cirrus codec datasheets as used on the ThinkPad T23). **Worktree:** `../CiukiOS-wt-f4-08` from `main`. Host-testable (register model); the lead qualifies on the T23 with an `L` capture.

## Owner report (2026-10-13, image 36010476)

"Audio 0": no sound at all on the ThinkPad T23, neither the system sounds nor the Control Panel "Test audio hardware" tone. On QEMU the AC'97 path plays (f4-audio pins). The quiet boot log shows only `[init] audio init done`.

## Task

1. **Real-hardware bring-up sequence**, datasheet-driven, in `src/kernel/drivers/ac97.c`: controller cold reset and codec-ready wait (Global Control/Status, `CR` bit, with timeouts), codec identification (vendor id registers 7Ch/7Eh, logged), power-down control (26h: clear the power-down bits, wait for the ready bits `REF/ANL/DAC/ADC`), **unmute and set volumes** (02h Master, 04h Headphone, 18h PCM Out, 0Ah PC beep if present: the power-on default of real codecs is muted, QEMU's is not), external amplifier (EAPD, 26h bit 15 on codecs that use it, or the vendor-specific register on others), sample rate (2Ch with VRA enabled in 2Ah; otherwise 48 kHz), the bus master registers (BDBAR, LVI, CR with RPBM/IOCE/FEIE), the interrupt line from the PCI config, and the ICH3-M specifics (secondary codec absent, S/PDIF off). Verify the mixer applies the master volume through the codec, not only in software.
2. **Diagnostics in `L` boot**: records `[audio] controller=.. codec=<vendor id> ready=.. power=.. master=.. pcm=.. rate=..` at init, `[audio] play=.. civ=.. picb=.. irqs=..` for the first second of the first playback, and a **boot tone**: when the loader menu selects `L`, play one second of 1 kHz at startup (after audio init) so the owner hears whether the codec works before any UI.
3. **Control Panel "Test audio hardware"**: report what the kernel saw (codec id, whether DMA advanced, IRQs received) in the dialog text instead of a generic result.
4. Tests: register model for reset/ready/power/unmute sequences (including a codec that keeps mute bits unless written twice, and one with EAPD), timeout and quarantine paths, record grammar; `f4-audio` host harness stays green.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, one host suite at a time. Report `build/codex/f4-08-report.md` with the specification references and the exact register sequence programmed.
