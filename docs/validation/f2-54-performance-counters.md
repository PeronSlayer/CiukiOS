# F2-54 compositor counters — host validation

Directive: `docs/directives/f2-54-performance-counters-after-f2-52.md`.
Source baseline: `9d6bb01e` (2026-10-14).

## Research and decision

Rechecked the [X11 CopyArea specification](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html#CopyArea)
and [upstream Savage XAA implementation](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c).
These describe rectangular screen copies separately from image uploads;
Savage exposes separate screen-copy, fill and CPU image-write operations and
an idle synchronization hook. Ciuki's counter definitions remain its own
contract: count successful rectangle operations as presents and CPU uploads
as LFB bytes, while counting each dirty band's area once.

Repository comparison: f2-52's `present_box()` uploads the first distinct
non-solid span and copies consecutive equal rows from already completed
scanout when the accelerator is available and scanout is valid. Each copy
increments `presents`, without adding CPU upload bytes or counting the dirty
area a second time. The failing performance fixture has no `d.accel`, so it
takes the single CPU-upload branch and never uses these repeated-row blits.

The stale clock expectation comes from f2-53's dynamic tray layout. In this
fixture the UI font measures `00:01:04` at 51 pixels and `00:01:05` at 50.
`desk_shell_clock_damage()` therefore calls `desk_shell_tray_damage()` and
repaints the taskbar to erase moved icons and resized task buttons. The
measured counters are legitimate; update the test rather than the presenter.

| Clock tick | Scene / damage pixels | Successful presents | LFB bytes |
| --- | ---: | ---: | ---: |
| 64 → 65 seconds, clock width changes | 32768 (1024 × 32) | 1 | 131072 |
| 65 → 66 seconds, clock width stays 50 | 1250 (50 × 25) | 1 | 5000 |

The test checks both ticks, including agreement between frame counters and
the independently recorded fake upload rectangles. Failures print expected
and actual damage, scene, present and upload counters. Existing accelerated
tests cover repeated-row screen copies, scanout equivalence and recovery.

## Qualification

Host suite results are recorded in `build/codex/f2-54-report.md`. No QEMU,
image build or hardware qualification is part of this directive.
