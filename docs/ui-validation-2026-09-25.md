# Native UI qualification, 2026-09-25

Status: **all eight listed candidate 5 qualifications passed; the user's
performance requirement remains incomplete**. Large 2K operations still take
seconds. This document distinguishes rendered/input checks in QEMU from claims
about physical hardware. It does not certify an ATI Radeon 9200 or the speed of
a real Pentium III.

## Test machine and evidence

Tests boot a private copy of `build/full/ciukios-full.img`. They do not write the
canonical image or any physical disk. The machine is QEMU, Pentium III CPU model,
one virtual CPU, **128 MiB guest RAM**, PS/2 keyboard and relative PS/2 mouse.
The complete QEMU command and payload SHA-256 hashes are recorded in each
`result.json`. KVM on the development host is used for functional checks; its
speed is not a measurement of a physical Pentium III.

Evidence root: `build/full/ui-redesign-2026-09-25/`.

| Run | Result | Scope |
| --- | --- | --- |
| `baseline/800` | Passed | Existing UI, 800×600, 128 MiB. Overlap, live drag, minimize/restore, keyboard focus, resize/maximize, cursor limits, DOS, Caps Lock, real COM program and return. Eight sampled opening frames, six distinct, no unrelated pixel changes. |
| `baseline/2560` | Harness failure retained | Booted 2560×1440/32-bit, overlap and six sampled drags passed. The old mouse-position helper could travel at most 2100 pixels within its attempt budget and failed a 2400-pixel journey. This run did not complete. |
| `baseline/2560-rerun` | Passed | Same original payload and 128 MiB, corrected bounded relative PS/2 travel. Twenty-four drag samples (2–3 distinct per step) and eight resize samples (7 distinct), all with zero changed pixels outside permitted damage. DOS/COM/desktop return passed. |
| `candidate-2/800` | Passed | Redesigned UI at 800×600, 128 MiB. Full interaction suite and Display panel. Eight opening samples (4 distinct), 24 drag samples (2–3 distinct per step), eight resize samples (4 distinct); zero unexpected outside pixels. |
| `candidate-2/2560` | Failed, retained | Same redesigned payload, 2560×1440, 128 MiB. Overlap, drag and cursor limits passed; maximize/restore was sampled with only the upper portion restored while the old full-screen body remained below. The harness did not wait for completion of that paint. This is visible non-atomic presentation, not evidence of persistent disk/kernel corruption. |
| `candidate-2/selection-negative` | Failed as expected | Clicking the next application left all 297 pixels of the old selection stripe unchanged. Actual stale highlight, independently reproduced. |
| `candidate-2/focus-negative-synced` | Failed as expected | Run → About → Alt+Tab → Enter closed Run rather than executing its typed command. Actual focus ownership defect, independently reproduced. |
| `candidate-2/release-negative-exposed` | Failed as expected | Pressing Run's close button and releasing on About's close button incorrectly closed Run. |
| `candidate-3/selection`, `focus`, `release` | Passed | All three corresponding negative cases now pass with real mouse/keyboard input and rendered assertions. |
| `candidate-3/idle` | Passed | 1024×768, 128 MiB. Four idle frames unchanged outside the clock; no paint markers in 2.06 seconds; 0.02 seconds QEMU host CPU time. |
| `candidate-3/maximize` | Harness comparison failure retained; latency measured | 2560×1440, 128 MiB. Completed maximize/restore took 2.743 / 2.638 seconds while 64 / 63 intermediate frames were captured. Final 65 changed pixels were exclusively the pointer, which ended two pixels away. Read-only analysis finds zero changed pixels outside both real cursor footprints and the clock. |
| `candidate-3/1024` | Passed | Full redesigned interaction suite at 1024×768, 128 MiB, all 40 sampled opening/drag/resize frames contained within permitted regions. Completed maximize/restore measured 218 / 236 ms on the host. |
| `candidate-4/maximize-atomic` | Passed visual atomicity; slow | Off-screen page presentation at 2560×1440, 128 MiB. All 56 maximize frames and 57 restore frames matched either the complete old or complete new image, excluding only the cursor and clock. Zero mixed frames and zero settled round-trip damage. Elapsed time remained 2.383 / 2.421 seconds. |
| `candidate-4/800` | Passed | Complete interaction suite for the shell with page presentation at 800×600, 128 MiB. Forty sampled opening/drag/resize frames remained within permitted damage; maximize/restore completed in about 199 / 186 ms. |
| `candidate-4/2560` | Harness synchronization failure retained | Display opened, but the old fixed 500 ms click delay ended before its completed paint. The later failure screenshot shows the intact Display window. |
| `candidate-4/2560-synced` | Harness focus assumption failure retained | Completed press/release synchronization allowed the suite to proceed. The old test assumed one Tab from Run reached Minimize, although the intended next control is Cancel. Enter therefore closed Run. Review also found production focus lookup ignored window ownership and duplicated title/footer close IDs, which could make this incorrect test pass accidentally in other modes. |
| `candidate-5/800` | Passed | Corrected focus ownership/control IDs. Full suite, 128 MiB, 40 sampled frames within permitted damage; complete DOS/COM/desktop round trip. Maximize/restore about 188 / 188 ms. |
| `candidate-5/2560` | Passed; performance remains open | Full suite at 2560×1440/32-bit, 128 MiB. All 24 drag and eight resize samples had zero outside changes; cursor, keyboard minimize and DOS/COM return passed. Maximize/restore about 1.77 / 1.78 seconds without the atomic capture burst. Ordinary large-window actions measured 1.6–2.3 seconds. Opening samples contained one distinct frame only. |
| `candidate-5/selection`, `focus`, `release` | Passed | Old highlight removed (297/297 stripe pixels), grid round trip identical; refocused Run executes the complete command; releasing on a different owner's close control retains both windows. |
| `candidate-5/keyboard-minimize` | Passed | At 2560×1440, two Tabs reach Minimize through Cancel; Enter retains a task button and restoring it returns the same window. |
| `candidate-5/idle` | Passed | At 1024×768, four idle frames unchanged outside the clock, zero paint markers over 2.061 seconds, QEMU host CPU 0.020 seconds (about 0.97%). |
| `candidate-5/maximize` | Passed atomicity; performance remains open | At 2560×1440, all 55 maximize frames and 56 restore frames were complete old/new images. Zero mixed frames and zero settled round-trip damage. Capture-instrumented elapsed times: 2.300 / 2.357 seconds. |

The eight actual exit codes are recorded in `candidate-5/batch-result.json`; all
are zero. `ui-validation-summary.json` records the final evidence and limitations.

### Source provenance

The immutable image used for the eight qualifications is `candidate-5.img`, with
SHELL.COM SHA-256
`4449cd0384077f5e81251dde24d87a79cbc0f0124419a3fcb1d6a067c6313d19`.
The later `development.img` shell has SHA-256
`82933833f649d7941a6cfc54da220d8d6232d4fb7e74fef30797e22530ee1acb`:
the integrator removed forced safe-video mode from the `boot_session .dos` path
used after Setup cancellation. The eight suites above were not silently
relabelled as tests of that changed binary. The integrator separately tested the
final CD's normal Live, Setup cancellation and DOS return paths at 128 MiB.

## What is actually checked

`scripts/qemu_test_native_windows.py` observes display pixels from QEMU and sends
real BIOS keyboard and relative PS/2 input. Serial markers synchronize waiting;
they are not accepted as evidence that a window was correctly rendered.

- Window dimensions are found from the rendered active title tab and outer bevel.
  Striped titles are supported without assuming a solid full-width title color.
- Opening Run preserves every sampled pixel outside its affected region, the
  previous focus decoration, the taskbar and the software cursor footprint.
- Run text survives opening an overlapping About window, repeated focus changes,
  moving About, closing it, minimizing Run and restoring it from its task button.
- Six successive held-button drag steps must produce at least five different
  actual window positions. Four frames per step are checked pixel-for-pixel
  outside the union of the old and new window rectangles. Eight resize frames
  receive the same check. The minute clock is excluded because it can change
  independently. Unexpected pixels are saved in magenta diagnostic images.
- Programs must visibly minimize, restore, maximize, restore its previous
  geometry, move and grow in both width and height. Its rendered frame may not
  overwrite the fixed taskbar.
- The intact software arrow must reach all four screen limits and immediately
  move back inward. The test rejects missing, duplicate and damaged cursors.
- F4 returns to DOS. ECHO must execute; two Caps Lock presses must not hang input;
  COMDEMO must actually print `COM demo via INT21h`; EXIT must restore the desktop
  and working pointer.

COMDEMO verifies a real external COM execution through the DOS API. It does not
certify every DOS executable, Windows, games or audio; those require their own
application-specific runtime checks.

Temporal checks prove the captured frames, not every possible refresh interval.
The report records frame counts, distinct frames and capture timing. Those times
include monitor protocol, screenshot transfer and host processing; they are not
guest FPS or an animation smoothness benchmark. A transition with only one
distinct sampled frame cannot establish how every intermediate frame appeared.

**Observed limitation:** candidate 3's full-width 2K maximize/restore took about
2.7 seconds on the development host and exposed intermediate horizontal bands.
Candidate 4's off-screen page presentation removes mixed frames in 113 actual
captured samples, but still takes about **2.4 seconds**. The visual corruption
and performance requirements are separate: correct atomic presentation does not
make that delay fluid, and the performance goal remains open.

## Repeatable commands

Baseline (existing palette):

```sh
python3 scripts/qemu_test_native_windows.py \
  --disk build/full/ciukios-full.img --memory 128 --profile 0800 \
  --output build/full/ui-redesign-2026-09-25/baseline/800-new-run
```

High-resolution candidate (use the actual candidate image supplied by the build):

```sh
python3 scripts/qemu_test_native_windows.py \
  --disk PATH/TO/CANDIDATE.img --memory 128 --profile AUTO \
  --video 2560x1440 --palette platinum \
  --output build/full/ui-redesign-2026-09-25/candidate/2560
```

`--video` explicitly exposes a 32 MiB VGA device with that preferred resolution.
The test then requires the captured desktop to have exactly those dimensions;
silently falling back to a lower resolution is a failure. Palette selection only
identifies rendered title and cursor pixels; it does not alter guest behavior.

## Remaining qualification

The full interaction suite has passed at 800×600 and 2560×1440 for candidate 5
(shell SHA-256 `4449cd0384077f5e81251dde24d87a79cbc0f0124419a3fcb1d6a067c6313d19`).
The 1024×768 full suite passed on candidate 3; current idle and targeted regressions
are listed separately. Setup interaction and low-memory installation require
separate tests. Large-window latency at 2K remains a failed performance goal.
Real hardware must confirm VBE modes, bank switching, cursor/keyboard behavior
and usable animation latency. QEMU VGA is not an emulation of an ATI Radeon 9200.
Fast typing while a long repaint is in progress is not yet qualified: the focus
regression intentionally waits for each character's paint to isolate focus
ownership from keyboard queue/latency behavior. Future responsiveness tests must
execute and compare an entire rapidly entered command, not just retained pixels.

The review also added `scripts/qemu_test_ui_regressions.py`, with isolated cases
for selection repaint, per-window keyboard focus, a press/release across controls
owned by different windows, settled idle CPU/display activity, and measured
maximize/restore presentation. Negative evidence is retained alongside subsequent
runs. The first focus test captured a pending earlier paint before F1 had been
processed; `focus-negative-synced` paces input through completed paint events and
reproduces the actual failure. The first cross-owner test assumed an incorrect
default window displacement; its replacement explicitly drags the second window
to expose both controls before testing release ownership.
