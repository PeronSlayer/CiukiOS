# f4-11 — Audio drivers, second set: Ensoniq ES1370/ES1371 (AudioPCI, SB PCI 128), Sound Blaster 16 ISA, AC'97 controllers of VIA, SiS and nVidia

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f4-11` from `main`, **after f4-10 is merged**. Host-testable; QEMU `es1370`, `sb16`, `AC97`.

## Task

Behind the F4 audio contract (`docs/design/f4-audio-media-contract.md`: one `/dev/audio` model, S16LE stereo 48 kHz default, bounded rings, silent DMA proof before `ready`, quarantine): `es137x.c` (ES1370 with its CODEC AK4531 and ES1371/ES1373/CT5880 with the AC'97 codec through the ES137x SRC; the most common PCI sound card family 1998–2001), `sb16.c` (ISA at 220h/IRQ5/DMA1,5 as the firmware or the PnP BIOS reports, 16-bit DMA playback; QEMU `sb16`), and AC'97 controller variants with the Intel-compatible NAM/NABM layout and their quirks: VIA VT82C686/VT8233/VT8235 (`via82xx`), SiS 7012, nVidia nForce/nForce2 (`intel8x0` in Linux covers them: cite the quirks). Device Manager names from hwids. Host tests with synthetic devices per driver in `tests/host/audio/`.

## Acceptance

`tests/host/audio/run.sh` green; i686 kernel compiles; the lead verifies tone playback on QEMU `es1370` and `sb16`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-11-report.md`.
