# f0-53 — the five red tests of the full host discovery on main e80c8f6c (gate of 2026-10-21, 1001 tests)

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-21 by the lead from the gate run on main e80c8f6c (canonical build green, image 2371ebed; `python3 -B -m unittest discover -s tests/host -v`: 1001 tests, 3 failures, 2 errors; log copied into this worktree as `build/gate-discover.log`).
- **Worktree:** `../CiukiOS-wt-f0-53`, branch `wt/f0-53`

Find the cause first; fix production or the harness, never weaken a check:

1. `test_normal_boot.NormalBootUnitTests.test_screen_heuristic_on_production_compositor_frames`: the host harness compiling `apps/desktop/client.c` (and companions) with `-fsanitize=address,undefined` fails at link (`clang: error: linker command failed`); rerun with `-v`/read the undefined references (likely symbols added by f7-07's `apps/desktop/client.c`/`protocol.c` changes or f2-87) and add the units or stubs to the harness rule.
2. `win32.dialogs.test_dialogs.DialogTests.setUpClass`: the dialog harness links its own GDI adapter, which lacks `host_pixel_format` (provider in `tests/host/win32/user32/gdi_host.c:191`): factor the helper into shared harness support used by both adapters. (The same item exists as f5-12B amendment 2 item 9 in a worktree not yet merged; your fix on main takes precedence, keep it minimal.)
3. `desktop.test_accessories.AccessoriesTests.test_catalog_registration` and `desktop.test_music.MusicTests.test_provenance_and_registration`: the tests expect the literals `"/system/licenses/stb_image.txt"` and `"/system/licenses/stb_vorbis.txt"` in `scripts/build_image.py`, which f0-50 moved into the `desktop_license_payloads()` loop. Make the tests check the licence inventory the build actually produces (import the function and assert the paths are present) instead of a source-text literal; keep the intent (every catalog entry's licence file is installed).
4. `test_audio.AudioWaveTests.test_selector_and_device_exception`: `'f4:vm-audio-jitter run=12345678' != 'f4:audio-native run=12345678'`: the selector/device-exception table changed (f5-21/f5-21b registrations or f0-50) so the case the test picks now resolves to another probe. Determine which is right: if the table gained a row that shifted an index, fix the test to select by name, not by position; if a registration overwrote `f4:audio-native`, fix the table.

Verification: each module green from `tests/host` the way the runner runs it (`cd tests/host && nice -n 19 python3 -B -m unittest <module>`), then `scripts/test/host_kernel_tests.sh` as far as the sandbox allows. Report `build/codex/f0-53-report.md`. `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; English.
