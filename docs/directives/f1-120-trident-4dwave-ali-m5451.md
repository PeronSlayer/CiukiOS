# f1-120 — Trident 4DWave DX/NX and ALi M5451 PCM engines

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f1-120` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions and the owner's licence decision at the end), binding. Read this directive's row, its class section and the device-model section completely. For audio, also read `docs/design/f4-audio-media-contract.md` and `docs/design/f4-audio-second-set.md`: the transport, mixer, DOS SB16 virtualisation and reporting rules every audio engine follows.

## Required

1. Write two separate engine modules, Trident 4DWave DX/NX and ALi M5451, with PCM playback and capture and their AC'97 codecs, on the f4 transport contract. Use the ALSA `trident` driver as donor.
2. Integrate with the DOS SB16 virtualisation.
3. Host tests cover each engine's register model, stop proof and quarantine.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, the donor driver files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-120-report.md`.
