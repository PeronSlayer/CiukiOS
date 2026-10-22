# f1-117 — EMU10K1/10K2 (SB Live!, original Audigy) stereo PCM and mixer

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-117` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions and the owner's licence decision at the end), binding. Read this directive's row, its class section and the device-model section completely. For audio, also read `docs/design/f4-audio-media-contract.md` and `docs/design/f4-audio-second-set.md`: the transport, mixer, DOS SB16 virtualisation and reporting rules every audio engine follows.

## Required

1. Write the EMU10K1/EMU10K2 voice engine for stereo PCM playback and capture, with the AC'97 codec mixer and a generated minimal DSP routing program, on the f4 transport contract. Use the ALSA `emu10k1` driver as donor.
2. Leave out the firmware-loaded variants that the source audit excludes from the Live!/Audigy PCM selection, with the reason recorded.
3. Integrate with the DOS SB16 virtualisation.
4. Host tests use the register, page-table and voice model: playback, rates, stop proof and quarantine.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, the donor driver files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-117-report.md`.
