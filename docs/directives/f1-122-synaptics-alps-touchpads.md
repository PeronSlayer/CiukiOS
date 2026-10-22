# f1-122 — Synaptics and ALPS touchpad absolute protocols (identified by query)

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f1-122` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions and the owner's licence decision at the end), binding. Read this directive's row, its class section and the device-model section completely. For audio, also read `docs/design/f4-audio-media-contract.md` and `docs/design/f4-audio-second-set.md`: the transport, mixer, DOS SB16 virtualisation and reporting rules every audio engine follows.

## Required

1. Identify Synaptics and ALPS touchpads by their documented query sequences, never by laptop branding, as the contract prescribes. Implement their absolute modes: tap, two-finger or edge scrolling, and a pass-through stick. Use the Linux `synaptics.c` and `alps.c` psmouse protocols as donors.
2. PS/2 compatibility mode stays the fallback on any failure. The f1-103 wheel and Explorer paths and the E500 firmware-first policy are kept.
3. Host tests use packet fixtures per protocol version, mode negotiation, the fallback and resync.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, the donor driver files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-122-report.md`.
