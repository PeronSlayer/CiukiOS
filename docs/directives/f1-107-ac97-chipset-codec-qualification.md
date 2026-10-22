# f1-107 — AC'97 qualification across chipsets and codecs (ICH, VIA, SiS 7012, nForce, ALi M5455)

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f1-107` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions and the owner's licence decision at the end), binding. Read this directive's row, its class section and the device-model section completely. For audio, also read `docs/design/f4-audio-media-contract.md` and `docs/design/f4-audio-second-set.md`: the transport, mixer, DOS SB16 virtualisation and reporting rules every audio engine follows.

## Required

1. Qualify the existing `ac97.c` routing and layouts for Intel ICH0–ICH5, VIA 686/8233/8235/8237, SiS 7012, nForce/nForce2/nForce3 and ALi M5455 (add M5455 only after its layout tests). Use the donor references the audit names, the Linux `intel8x0` and `via82xx` drivers.
2. Add a codec name and capability table: vendor id, variable rate, mixer masks, EAPD and jack quirks for common codecs (AD1881/1885/1886/1980/1981/1985, ALC650/655/658/850, CS4299, STAC97xx, CMI9738/9739, VT1616, WM97xx). Wire it to Device Manager and to failure reasons.
3. Host tests use controller register models per layout, codec probe fixtures and refusal paths.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, the donor driver files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-107-report.md`.
