# fx-w32green: make the two known Win32 host failures pass

- **Written:** 2026-10-28 18:42 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: main (4cba53067 or later).
- **Evidence:** both failures appear in every recent integration run of `python3 -B -m unittest discover -s tests/host/win32 -t .`:
  - `build/lead-scratch/chain-integ5/win32.log`;
  - `build/lead-scratch/chain-integ7/win32.log`.

  The 1.0 Beta needs a green Win32 suite.

## Defects

1. **`tests.host.win32.media.test_media.Media.test_format0_header_compatibility`.** It fails with `classaudit_midi.c:27: Assertion 'diagnostics==3' failed`.
   - The test feeds format-0 SMF headers with track counts 0, 1, 2 and 0xffff to the production loader `sdk/win32/audio/mciseq.c` `load()`. It expects one `format0_track_count` diagnostic for each count other than 1.
   - Find out whether the loader or the test is wrong. The reference is Wine's `mciseq` (`dlls/mciseq/mcimidi.c`, MIDI_mciReadMThd) and the Standard MIDI File 1.0 specification.
   - Fix whichever is wrong, and record the source in the media design document.
2. **`tests.host.win32.test_census.CensusTests.test_committable_census_is_reproducible_and_current`.** The generated census differs from `docs/design/win32-census/fixtures.txt`: it lists `pe-signature` where the committed file lists `pe-offset-alignment`.
   - Find out which change to the fixture corpus or the census tool caused this. Related failures on some bases name the fixture `bad-pe-offset-alignment` in `test_loader`/`test_kernel_loader`.
   - Decide whether the committed census or the generator is right, from the PE/COFF specification (`e_lfanew` alignment and bounds) and the loader's refusal manifest.
   - Fix the wrong side and regenerate the committed file with the census tool. Do not hand-edit it.

## Acceptance

- Both tests pass.
- The whole Win32 suite gains no failure. Failures that only LeakSanitizer causes in the sandbox are listed separately; the lead reruns them.
- `python3 scripts/gen_api_reference.py --check` passes.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-w32green` (branch `wt/fx-w32green`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/fx-w32green-report.md`. Leave the changes uncommitted.
