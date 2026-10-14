# f4-18 — DOS games too quiet, boot volume at 20 %, ThinkPad volume keys drive the system volume

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f4-18` from `main`. Host-testable; the lead runs `f4-audio` on QEMU and the T23 test.

## Evidence (T23, image fb3147dc, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean`)

- `[vm-audio] vm=1 gain=100,100 mute=0 master=50,50 master_mute=0 codec_master=1` and `ct_master=24,24 voice=24,24 fm_gain=24,24 output_gain=0,0`: the SB16 mixer power-on defaults (0x18 = 24/31 in registers 0x30-0x35) are applied as attenuation (2 dB per step: −14 dB master plus −14 dB voice) on top of a fixed VM master of 50 %. Owner: DOOM's sound is audible but far below the system sounds. DOSBox maps the 5-bit registers linearly (`soundblaster.cpp` CTMIXER: `(reg>>3)/31.0`, verify) so defaults play near full scale, which is what users expect.
- System master default `left=50,right=50` (`src/kernel/audio/audio_dev.c:46`); owner: boot at a low level, about 20, unless the user configured otherwise.
- Owner: the ThinkPad T23 volume keys (volume up/down/mute buttons next to the power button) must change the system volume.

## References

Creative Sound Blaster 16 Hardware Programming Guide (mixer registers 0x04/0x22 legacy 4-bit, 0x30-0x35 5-bit, 0x41/0x42 output gain 0-18 dB), DOSBox `src/hardware/soundblaster.cpp` CTMIXER code (behaviour only), the registry contract `docs/design/registry-contract.md` (where user settings live), Linux `drivers/platform/x86/thinkpad_acpi.c` volume subdriver (`TP_EC_AUDIO` 0x30: bits 0-3 level 0-14, bit 6 mute; `TPACPI_VOL_Q_LEVEL` quirks that list which models have the EC-driven hardware mixer: state whether the T23 ("1A" EC id?) is among them), `tp_smapi`/`thinkpad_ec` for the EC access path on pre-2005 ThinkPads, ACPI specification §12.2 (EC command 0x80 read / 0x81 write on ports 0x62/0x66, IBF/OBF handshake), the T23 DSDT `EC0` `_CRS` (ports 0x62/0x66) if findable online.

## Task

1. **SB16 mixer mapping** (`src/kernel/vm/dosvm_sb16.c`, `src/kernel/audio/vm_source.c`): linear mapping of master/voice/FM/CD like DOSBox, output gain as +6 dB steps capped at the mixer's headroom, legacy 4-bit registers likewise; a full-scale 8-bit DMA stream at mixer defaults must peak within 3 dB of a full-scale system sound at the same system master (host test with the vm audio harness `tests/host/dosvm_vmaudio_test.inc`). The VM master follows the system master (no fixed 50; keep `codec_master` semantics).
2. **Boot volume**: default system master 20 % unless the registry holds the user's value (key per the registry contract; Control Panel writes it, `apps/control`); boot tone and UI sounds use the configured level; host tests for the default and the persisted value.
3. **ThinkPad EC volume** (`src/kernel/drivers/thinkpad_ec.c`, new): enabled only when the machine identifies as an IBM ThinkPad (SMBIOS/DMI if the kernel parses it — check `bootinfo.c`; otherwise the hardware profile detection at boot) and the EC answers the ACPI handshake; poll register 0x30 at 4 Hz; map level 0-14 to master 0-100 and the mute bit; push to the mixer; the desktop shows its volume indicator (reuse the Control Panel/tray volume UI, no new widget). Records `[ec] thinkpad volume=<n> mute=<0|1>` at boot and on change. If the research shows the T23 attenuator is in the analog path (the keys change loudness in hardware), do not double-attenuate: on such machines the master follows the EC level and the codec master stays at 0 dB; document the finding in `docs/design/hardware-baseline.md`.
4. QEMU has no ThinkPad EC: the driver stays off there and the host test uses an EC model.

## Acceptance

`tests/host/test_audio.py`, `tests/host/audio`, dosvm audio host tests, control panel tests green; the lead runs `f4-audio` on QEMU and the T23: DOOM as loud as the system sounds at the same master, boot at 20 %, the volume keys move the system volume. Files: `dosvm_sb16.c`, `src/kernel/audio/*`, new `thinkpad_ec.c`, `apps/control`, registry defaults, desktop indicator only.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-18-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
