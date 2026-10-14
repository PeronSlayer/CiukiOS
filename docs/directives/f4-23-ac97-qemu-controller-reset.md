# f4-23 — AC'97 bring-up fails on QEMU at `controller_reset` since the ICH3 work (f4-15): the mixer reads 0xFFFF

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f4-23` from `main`. Host-testable; the lead runs `f4-audio` on QEMU.

## Evidence (QEMU `qemu-t23` profile, image 93a04a4c of main 3ebc9e26, run `build/test-runs/f4-audio/701dd2c6`)

```
[audio] stage=pci_decode ... result=ok access=ok
[audio] stage=controller_reset reg26=0000 reg02=0000 reg18=0000 valid=00 cnt=00000000 sta=00000000 cas=00 codec=00000000 time=4298 result=failed access=ok
[audio] controller=8086:2415 codec=00000000 ready=00000000 power=0000 master=0000 pcm=0000 rate=0 irq=11 error=-5 reason=controller_reset
[audio] failure stage=controller_reset ext2a=ffff rate2c=ffff pisr=0001 posr=0001 mcsr=0001 pocr=00 civ=0 lvi=0 piv=0 picb=0
```

The `audio-native-qemu-t23` case fails ("missing probe requires READY then ERROR", `READY case=backend status=not_run reason=controller_reset`). QEMU's device is the ICH `8086:2415` (`hw/audio/ac97.c`); on the T23's ICH3 `8086:2485` the same driver reaches `stage=ready` (boot tone audible on image 445e91c7). The last green `f4-audio` on QEMU predates f4-15 (ICH3 CIV/LVI/PICB semantics, cold reset with one-second primary-ready wait of f4-10) and f4-18 (codec attenuation gated by `master_in_hardware`); the suite was not run on QEMU between them. Mixer registers reading 0xFFFF during the reset stage means the NAM BAR is not answering: in QEMU's model, check what `nam_readw`/`nabm` return while `GLOB_CNT` cold reset is asserted or before the codec is "ready", and what the primary-codec-ready bit needs.

## Task

1. Read QEMU `hw/audio/ac97.c` (`nam_readw`, `nabm_writel` for `GLOB_CNT`, `mixer_reset`, `GLOB_STA` primary ready) and the Intel 82801AA/ICH3 AC'97 datasheet reset sequence; find which step of `ac97_controller_reset` (`src/kernel/drivers/ac97.c:83`) QEMU does not satisfy (cold-reset bit polarity, read of 0x2A/0x2C before ready, the one-second wait, warm vs cold reset) and make the sequence correct for both the real ICH3 and QEMU's model without weakening the T23 path (f4-09/f4-10/f4-15 decisions stay; record which path ran).
2. Host model: extend the AC'97 model in `tests/host/audio` with QEMU's behaviour (0xFFFF mixer reads until the cold reset is released and the codec reports ready) and prove the driver passes both models.
3. Report the exact divergence and the datasheet/QEMU references in `docs/design/f4-audio-media-contract.md` or the validation record of f4-15.

## Acceptance

`tests/host/audio/run.sh` and `test_audio.py` green; the lead runs `f4-audio` on QEMU (`audio-native-qemu-t23`, `-desktop-2002`) and the T23 must still reach `stage=ready` with the boot tone. Files: `src/kernel/drivers/ac97.c`, `tests/host/audio`. Do not touch the USB stack (f1-59), `thinkpad_ec.c` (f4-21) or `savage2d.c` (f4-22).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`), record links and decision, then implement and test. Report `build/codex/f4-23-report.md`.
