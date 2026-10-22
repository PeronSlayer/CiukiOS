# f1-116 — ESS Maestro 1/2/2E native PCM engine and the E500 routing

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-116` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions and the owner's licence decision at the end), binding. Read this directive's row, its class section and the device-model section completely. For audio, also read `docs/design/f4-audio-media-contract.md` and `docs/design/f4-audio-second-set.md`: the transport, mixer, DOS SB16 virtualisation and reporting rules every audio engine follows.

## Required

1. Write an ESS Maestro 1, 2 and 2E PCM playback and capture engine with its AC'97 codec and mixer, on the f4 transport contract, with the Linux `maestro` OSS driver and ALSA `es1968` as donors. Include the E500 (Armada) routing and EAPD/amplifier qualification.
2. Integrate with the DOS SB16 virtualisation as the other engines do.
3. Host tests use the engine register and APU model: playback, rates, underrun and stop proof, and quarantine.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, the donor driver files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-116-report.md`.
