# 101-b DOS video acceleration report

Written 2026-10-25T10:26:35+01:00 after running `date -Iseconds`.
Worktree: `/home/peronslayer/Desktop/CiukiOS-wt-101-dosvid`, branch
`wt/101-dosvid`. Baseline: `377e660f5783c74122fa6da877c86dfb3ae3aa26`.

**Status: partial directive implementation, ready for lead review; acceptance
is not fully green.** The software/available native-COPY optimizations are
implemented and pixel-checked. True native stretch, direct VBE mapping and
physical per-stage timing remain open. Several required sanitizer runs return
failure because this sandbox denies LeakSanitizer's ptrace operation.

## Implemented

- Per-frame palette deltas restrict VGA damage to rows using changed colours.
  Mode 13h and Mode X palette fades skip shared-surface readback. DAC writes
  remain batched by the existing fx-27 monitor; no conversion is performed per
  palette component write.
- VBE8 retains the sampled indexes in the existing VGA row cache. Equal
  dirty-page rewrites skip conversion, and changed rows skip surface readback.
  VBE16 no longer redraws for unrelated DAC changes. BIOS 4F09 updates respect
  the existing frame deadline instead of forcing an immediate full repaint.
- Fit and 100% viewport rows use memcpy plus clamped-edge fill. 125% resolves
  horizontal coordinates once per paint. Selection, scroll bounds, overlays,
  exact changed scanlines and the software fallback preserve their pixels.
- The generic desktop COPY/FILL path now handles its first scaled DOS frame.
  COPY reads a successfully presented predecessor in the same call. A private
  flag preserves one full CPU repaint after a display-lease generation change.
  Submitted-copy failure still reconstructs from retained client pixels.
- New host regressions and optional host benchmarks cover mode-13h palette
  fades, real 320x240 unchained Mode X page flips, VBE8/16, palette masks,
  BIOS palette cadence, viewport pixels and native/software presentation.

No new panic paths, public ABI/SDK changes, vendored code, image builds,
QEMU, systemd scopes, Git mutations or dev_diary edits were made. The private
`apps/desktop/desktop.h` field does not change the SDK or wire protocol.
The conditional ABI-oracle/API-reference and vendored-symbol rules therefore
were not triggered.

## Research and decisions

The persistent comparison with the implementation is in
`docs/validation/101-dos-video-accel.md`; the full display-model contract and
fx-27 validation record were read before changing the path.

| Primary source checked | Resulting decision |
| --- | --- |
| Windows 98 Resource Kit, chapter 24, “Display Driver Overview”, read-only `/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc24_3.htm`, lines 202–214 | Keep memory-surface drawing in the DIB/software role and dispatch supported screen operations through the minidriver. No Resource Kit text or media was copied. |
| [Microsoft: Windows 95 grabbers](https://devblogs.microsoft.com/oldnewthing/20231127-00/?p=109067) | Retain VDD video virtualization, comparison of changed data and incremental window painting. This concerns Windows 9x V86 sessions, not NT NTVDM. |
| [QEMU 10.1 VGA source](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c), `update_palette256`, `vga_draw_graphic` | Keep palette processing at refresh and dirty-page scanline updates. Refine damage for actually used palette entries. This source was read; QEMU was not run. |
| [X.Org Savage XAA](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c), [engine source](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c) | The reviewed 2D interfaces provide copy, fill, image write and monochrome expansion. They do not establish a stretch hook; no registered CiukiOS driver advertises native SCALED. Do not claim the chip lacks all scaling hardware, or program an unqualified overlay/3D path. |
| [X.Org EXA interface](https://raw.githubusercontent.com/XQuartz/xorg-server/master/exa/exa.h) | Keep source/destination synchronization, synchronous completion and the existing COPY failure/reconstruction path. |

The supplied directive was read completely and still contains 14 lines; it
has no appended amendments or lead notes in this worktree.

## Per-frame path and target-model limits

Windowed VGA: trapped VGA I/O → PTE/plane dirty collection → retained index
comparison → palette expansion and integer scaling into 640x480 XRGB → VM
shared surface → dosbox viewport/application surface → desktop composition →
generic display present/native COPY/FILL. VBE substitutes pinned linear RAM
and page dirt for the VGA-plane stage.

| Stage | Mode 13h 320x200 | Mode X 320x240 | VBE8 640x480 |
| --- | --- | --- | --- |
| Windowed VGA I/O | Existing lean exits and DAC burst; unchanged by 101-b | Existing lean plane/CRTC exits and DAC burst; unchanged | Existing palette traps/BIOS calls; unchanged |
| Dirty tracking | 16 aperture PTEs plus retained indexes; no chain-4 deinterleave for this path | Plane-page dirt and retained displayed indexes; equal flips redraw zero rows | Pinned-buffer PTE dirt; only sampled dirty rows examined |
| Changed full frame: source bytes converted | 64,000 | 76,800 | 307,200 |
| Image rows / XRGB bytes written to VM surface | 400 / 1,024,000 | 480 / 1,228,800 | 480 / 1,228,800 |
| Fit application surface | Up to 640x452 pixels / 1,157,120 bytes; selected rows only | Same fixed viewport | Same fixed viewport |
| Desktop | Exact damage, clipped composition, supported COPY/FILL or CPU fallback | Same | Same |
| Full screen | Direct VGA aperture and native port grant; no window conversion/composition | Same native VGA path | Private RAM plus dirty-page provider writes; 307,200 bytes / 75 pages for a fully dirty frame; no RGB expansion/window compositor |

Full-screen transition capture/restore and watchdog work are separate from
steady-state per-frame costs. VBE currently has no provider export for a
validated guest-mappable physical LFB, and its backing pages belong to the
VM pool. Removing the copy needs a contract for physical extent/cache flags,
exclusive ownership, revocation, restoration of borrowed PTEs, pool/sampler
accounting, kernel access and screenshot capture. No guessed physical grant
was added.

The directive's QEMU sample, 108,879 exits / 334,859 us, is approximately
3.0755 us/exit **on that QEMU run**, not on the PIII-M. The prior T23 figures
in `docs/validation/fx-27-windowed-dos-graphics.md` attribute approximately
12–16 ms/frame to windowed Quake monitor/presenter work after fx-27, with
stated modelling assumptions. This change does not remeasure those hardware
times or the unchanged traps. There is no new measured PIII-M FPS or complete
trap/dirty/palette/scale/copy/compositor timing decomposition.

For the synthetic first Fit desktop frame, CPU transfer volume falls from
1,920,000 to 822,400 bytes at 32 bpp (57.2%), with 273 COPY and 15 FILL calls.
Using the existing host fixture's **134 MB/s bus assumption**, transfer-only
cost is 14.33 → 6.14 ms. These are modelled bus times; engine submission,
engine completion, conversion, composition and actual silicon timing are extra.
This estimate is not a measured frame-time improvement. At 16 bpp the byte
counts halve; the bandwidth and rendering cost must be measured again.

## Before/after evidence

The same new tests were compiled with baseline and changed production files.
`CIUKI_101_BASELINE=1` makes budget failures nonfatal only in the baseline
harness; normal tests enforce them. Eighteen VM budgets pass after the change;
sixteen fail before. Two unchanged controls also pass before.

| Deterministic per-frame work | Before | After |
| --- | ---: | ---: |
| 13h unused palette: source bytes converted | 64,000 | 0 |
| 13h unused palette: surface read calls | 4,000 | 0 |
| 13h fade: source bytes converted | 64,000 | 64,000 |
| 13h fade: surface read calls | 400 | 0 |
| Mode X equal page flip: source bytes converted (control) | 0 | 0 |
| Mode X palette fade: surface read calls | 480 | 0 |
| VBE8 unused palette: source bytes converted / surface reads | 307,200 / 4,800 | 0 / 0 |
| VBE8 fade: surface read calls | 480 | 0 |
| VBE8 BIOS palette: premature frame before deadline | 1 | 0 |
| VBE8 changed full frame: surface read calls | 480 | 0 |
| VBE8 equal full-frame rewrite: source bytes converted | 307,200 | 0 |
| VBE8 changed masked-out colour: source bytes converted | 307,200 | 0 |
| VBE16 unrelated palette: source bytes converted / surface reads | 768,000 / 4,800 | 0 / 0 |
| First Fit desktop frame: CPU-uploaded pixels | 480,000 | 205,600 |

“Converted bytes” counts source bytes expanded into RGB, not every index
comparison or RAM read. In particular, a VBE8 palette-only sample may still
read source rows to establish their palette dependency. A forced redraw is
compared byte-for-byte with the retained regression frames. Additional existing
VGA vectors cover pan, split, wrapping, masks, mode changes and dirty pages.

Host timing samples below use identical `-O1` ASan+UBSan compilation, nice 19,
64 synthetic frames and CLOCK_MONOTONIC, excluding synthetic guest stores and
port setup. Samples finish before the LeakSanitizer exit failure. Timings are
descriptive, not test assertions or a hardware model.

| Presenter + surface write sample | Before, us/frame | After, us/frame |
| --- | ---: | ---: |
| 13h palette fade | 219.701 | 202.059 |
| Mode X 320x240 changed-page flip | 358.054 | 344.127 |
| VBE8 full frame | 356.303 | 357.434 |

The full VBE8 sample is effectively unchanged/slightly slower on this host;
no throughput gain is claimed for that workload. Equal frames and palette-only
updates reduce deterministic work. Host timing varies with cache and load.

Separate viewport-only samples use the production dosbox painter, full dirty
rows, the same sanitizer flags and 64 paints; all final pixels match the
scalar coordinate oracle, including the scrolling/selection regression.

| Viewport | Before, us/frame | After, us/frame |
| --- | ---: | ---: |
| Fit | 805.956 | 32.020 |
| 100% | 842.798 | 35.115 |
| 125% | 880.543 | 382.204 |

## Verification

All test commands below were sequential at nice 19. No caller-supplied
ASAN_OPTIONS or LSAN_OPTIONS was added. Existing script sanitizer policy was
preserved, including scripts which already set `detect_leaks=0`. Where default
leak detection is enabled, the sandbox reports “LeakSanitizer has encountered
a fatal error” and explains that it requires ptrace. Such commands have exit
1 and are explicitly **not accepted as green**. No sanitizer memory/UB report
or assertion failure precedes those final exit-check errors.

| Exact command | Result |
| --- | --- |
| `nice -n 19 bash build/codex/101-build-vm.sh` | PASS; canonical host-script DOS VM compile slice, ASan+UBSan, no warnings. Log: `101-build-vm.log`. |
| `nice -n 19 env CIUKI_VM_101_VIDEO=1 build/host/dosvm_test` | Exit 1: all 18 budgets and forced-redraw pixel comparisons pass; LeakSanitizer fatal at exit. Log: `101-vm-101.log`. |
| `nice -n 19 env CIUKI_VM_FX27=1 build/host/dosvm_test` | Exit 1: all 7 fx-27 budgets and 622,980 checks pass; LeakSanitizer fatal at exit. Log: `101-vm-fx27.log`. |
| `nice -n 19 build/host/dosvm_test` | Exit 1: body PASS, 60,213,209 checks; LeakSanitizer fatal at exit. Log: `101-vm-full.log`. |
| `nice -n 19 env CIUKI_VM_101_BENCH=1 build/host/dosvm_test` | Exit 1: all three 64-frame samples complete; LeakSanitizer fatal at exit. Log: `101-vm-bench.log`. |
| `nice -n 19 python3 -B tests/host/test_display_driver_model.py` | PASS, 10 tests; 22 descriptors, 396 exclusive PCI pairs, 54 sources. Log: `101-display-model.log`. |
| `nice -n 19 python3 -B tests/host/test_display_commands.py` | PASS, 3 tests: software pixels, registry/batch dispatch, SDK wrappers and desktop commands. Log: `101-display-commands.log`. |
| `nice -n 19 python3 -B tests/host/test_display_driver_names.py` | PASS, 4 tests. Log: `101-display-names.log`. |
| `nice -n 19 python3 -B tests/host/test_presenter_display.py` | PASS, 4 tests. Log: `101-presenter-display.log`. |
| `nice -n 19 python3 -B tests/host/test_vga_x86.py` | PASS, 5 tests. Log: `101-vga-x86.log`. |
| `nice -n 19 bash build/codex/101-vga.sh` | Exit 1: 173,117 independent VGA assertions pass; LeakSanitizer fatal at exit. Log: `101-vga-model.log`. |
| `nice -n 19 python3 -B tests/host/desktop/test_dosbox_properties.py` | Exit 1: full G17 body and new full/incremental viewport pixels pass; LeakSanitizer fatal at exit. Log: `101-desktop-properties.log`. |
| `nice -n 19 python3 -B tests/host/desktop/test_accel.py` | PASS, 5 tests, including native register fixtures, cursor goldens, DOS Fit first-frame copies, failure reconstruction and lease recovery. Log: `101-desktop-accel.log`. |
| `nice -n 19 python3 -B tests/host/desktop/test_compositor_performance.py` | PASS, 2 tests, both theme goldens and row/clipping oracle. Log: `101-desktop-compositor.log`. |
| `nice -n 19 python3 -B tests/host/test_kernel_stack.py` | PASS, 4 tests; native files payload fixture generated first (no kernel/image build). Log: `101-kernel-stack.log`. |
| `nice -n 19 bash tests/host/vm/run.sh` | Exit 1: all component binaries compile; script stops at bridge_test LeakSanitizer fatal. Later components were not executed by this script. Log: `101-vm-run.log`. |
| `nice -n 19 bash tests/host/vm/run_lifecycle.sh` | Exit 1: compile succeeds; LeakSanitizer fatal at runtime exit. Log: `101-vm-lifecycle.log`. |
| `nice -n 19 bash tests/host/vm/run_process_exit.sh` | Exit 1: compile succeeds; LeakSanitizer fatal at runtime exit. Log: `101-vm-process-exit.log`. |

Additional exact commands:

| Exact command | Result |
| --- | --- |
| `nice -n 19 python3 -B build/codex/101-baseline.py` | Baseline VM compile PASS; 16/18 budget failures as expected, pixel checks complete; VM/benchmark exit 1 from LeakSanitizer. Baseline desktop suite PASS with budget comparison enabled only for reporting. Source files restored by the helper. Logs: `101-vm-before.log`, `101-bench-before.log`, `101-desktop-before.log`. |
| `nice -n 19 python3 -B build/codex/101-view-baseline.py` | Viewport timing and pixel body complete against original painter; exit 1 from LeakSanitizer. Log: `101-view-before.log`. |
| `nice -n 19 env CIUKI_101_VIEW_BENCH=1 python3 -B tests/host/desktop/test_dosbox_properties.py` | All viewport samples and G17 body complete; exit 1 from LeakSanitizer. Log: `101-view-after.log`. |
| `nice -n 19 python3 -B build/codex/101-stack-fixture.py` | PASS; generated only `proc_abi.inc` and native `files-payload.elf` fixture from the repository's ABI dumper/NASM recipe. Fixed the initially missing fixture; stack suite then passes. |
| `nice -n 19 python3 -B build/codex/101-target-build.py` | PASS. Both changed kernel units compile with exact kernel CFLAGS, including 4096-byte frame bound. All production dosbox/desktop application sources compile and both ELFs link with `--no-undefined` and `-Werror`, using the lead's installed SDK and libdeskui read-only. All outputs and temporary files remain in this worktree. Full command trace: `101-target-build.log`. This is not a complete kernel link or image build. |
| `git diff --check` | PASS. |

The initial first-frame budget was corrected to include the surrounding
800x600 desktop/chrome rather than assuming only guest-image bytes. The final
budget requires less than half of a full desktop upload. A real display lease
change retains the existing complete CPU repaint regression. The complete
final desktop acceleration suite passes; the earlier failing development
runs are superseded by it.

The helper scripts are session scratch material in `build/codex`, not new
repository scripts. The host compile helper is the DOS VM slice extracted
from `scripts/test/host_kernel_tests.sh`; the VGA helper uses that script's
standalone VGA compile command. Complete exact target compile/link arguments
are in the target-build log. No public header changed, so no ABI-oracle update
or API-reference regeneration was necessary. No upstream code was vendored,
so the conditional included-C symbol-duplication check was inapplicable.

## Decisions needed and remaining items

1. **Native stretch.** Decide the follow-up minidriver operation and hardware
   qualification for true SCALED/colour expansion of DOS frames. No current
   native driver supplies SCALED. This patch uses available COPY/FILL for
   presentation and preserves software scaling; it does not provide a native
   stretch backend or a native stretch-vs-software equivalence result.
2. **Direct VBE full screen.** Specify/review the provider mapping/revocation
   contract described above before replacing VM-owned RAM with borrowed MMIO
   PTEs. VGA aperture access is already direct. Direct VBE is not implemented.
3. **Target measurements.** The lead must measure the individual stages on
   the target image/hardware, including native command overhead, Fit and
   full screen, all three graphics layouts and both fallback/engine states.
   No new target image SHA or physical performance evidence exists here.
4. **Sanitizers and integration.** Rerun the default-leak-detection VM, VGA,
   G17 and VM-network/lifecycle scripts in an environment allowing their exit
   check. `tests/host/vm/run.sh` did not complete its runtime component chain.
   The lead still owns complete kernel linking, the canonical image build,
   cross-review, integration and any later allowed runtime qualification.

The worktree contains code/tests and the persistent validation note for
review. The directive remains untracked as supplied. Nothing was staged,
committed, merged or pushed.
