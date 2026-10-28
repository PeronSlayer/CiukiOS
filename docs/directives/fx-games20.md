# fx-games20: twenty redistributable DOS games with sound, full screen and LAN

- **Written:** 2026-10-28 12:25 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: main.
- **Goal (1.0 Beta):** twenty DOS games of the period play on CiukiOS with sound, full screen by default (fx-dosfull), mouse and joystick where they use them, and, where the game supports it, IPX network play between machines. The owner's decision of 2026-10-28 (dev diary 2026-10-28-01) limits the image and the tests to shareware, demo and freeware packages.
- **Binding specification:**
  - `config/dos-titles.json` and its classes (`bundled`, `test-media`) with the per-title terms records;
  - `third_party/games/*/UPSTREAM.json`;
  - `docs/directives/fx-dosfull.md`;
  - `docs/directives/f6-11-dos-ne2000-ipx-bridge.md`;
  - `tests/suites/acc-t23.json` and `scripts/test/acceptance.sh`.

## Scope

1. **Twenty titles.** Bring `config/dos-titles.json` to twenty titles: today's six plus fourteen more. Choose period DOS games (1990–1998) whose official shareware, demo or freeware packages are downloadable from their publishers or from long-standing archive mirrors, with pinned URLs and SHA-256 in `build/downloads/dos-titles`.
   - Cover several genres and the hardware paths the beta must prove:
     - VGA Mode 13h and Mode X;
     - VESA SVGA;
     - DOS/4GW and DPMI;
     - Sound Blaster digital, OPL and General MIDI music;
     - mouse;
     - joystick;
     - IPX multiplayer (at least six titles with IPX).
   - Record each title's terms the way the existing entries do, and its class: `bundled` when the package allows it, `test-media` otherwise. Bundled titles get Start menu Games entries with their own icons (fx-nativeicons, fx-exeicons).
2. **Acceptance cases**, one per title, in `tests/suites/acc-t23.json`. Each case:
   - launches from the Games menu;
   - asserts full screen and sound activity from serial records (Sound Blaster DMA or OPL writes);
   - sends the title's input to reach gameplay;
   - quits back to the desktop with the VM cleaned up.
3. **LAN cases** for the IPX titles. Two guests on one QEMU socket or multicast netdev; the owner allows up to three QEMU only for LAN tests.
   - Host and join over the DOS IPX bridge, and assert from both serial logs.
   - Write the runner support for multi-guest cases: start, network, collect, and stop all guests on any failure.
4. **Host tests:** the parser and fixture wiring of the new cases, and the download/pin manifest checks. Never commit game bytes.

## Acceptance

- The host tests pass.
- The desktop (`python3 -B -m unittest discover -s tests/host/desktop -t .`), Win32 and kernel suites gain no failure against the base.
- `scripts/test/acceptance.sh` lists every new case.
- The lead runs the QEMU cases and the canonical build.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-games20` (branch `wt/fx-games20`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Downloads go only to `build/downloads/dos-titles`. If your sandbox has no network, write the URLs and the existing fetch path, and leave the SHA-256 fields marked `lead-pins`. The lead downloads the packages, pins them and reruns your manifest checks.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/fx-games20-report.md`. Leave the changes uncommitted.
