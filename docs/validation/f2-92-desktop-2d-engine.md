# f2-92: the desktop draws with the 2D engine

Written 2026-10-22. Host-tested; T23 acceptance remains open. Design and
upstream sources: [Savage 2D, f2-92](../design/savage-2d.md#f2-92-the-display-service-draws-with-the-engine).

## Evidence and finding

T23 image 5a10eef4 (`build/codex/t23-5a10eef4-serial.txt`): the self-test
passed (`bci_test result=ok … phase=done pixels=16`, line 224), `caps=3`
(line 225) and `[accel] summary accelerated=1 driver=savage reason=none`
(line 259). The activation finished after `[supervisor] desktop pid=2`
(line 165): the desktop had already started with no capabilities. The
compositor re-read the capability mask each frame but loaded the hardware
cursor only on a mode-generation change, and its window-move and scroll
copies require the hardware cursor, so no window move ever used the engine.
No record showed engine use (the `ops=` diagnostics need `L`). The
completion wait was bounded at 256 polls of 5 µs (about 1.5 ms), below what a
large fill or copy can take, and a drawing failure quarantined without a
record.

## Host evidence

- `tests/host/savage2d_test.c` (register model, 32 and 16 bpp): clipped host
  write `4bcc4040`, `CLIP_LR` = clip columns, only clipped words sent, exact
  pixels, untouched surroundings; queue filled to exactly 32 entries, never
  beyond, with 5 entries drained per status read; offscreen write then
  offscreen-to-visible and visible-to-offscreen copies; the gap below the
  screen, rows past the lease and straddling rectangles rejected; a 5 ms
  engine waited in 5 slices, each with the presenter lock released, the
  drawing claim kept and a mode change refused; a 30 ms engine quarantined
  after the 20 ms deadline and the CPU finished the fill after the idle proof;
  a stuck engine and a never-draining queue stop within 2x20 ms; the write
  self-test passes and restores the block, a byte-replicating image disables
  writes and offscreen rows, a dropped offscreen write disables offscreen rows
  only, fills unaffected; T23 offscreen span rows 768-2047; the production
  instance reports `used=0` until the first engine draw, `used=1` once, and a
  drawing failure records `quarantined … phase=draw` and flips the summary.
- `tests/host/desktop/accel_test.c`: every frame equal to the software oracle
  with the hardware-cursor, fill, copy and offscreen-cache paths, including
  window moves, raises, the Start menu, client content changes, a background
  change, a closed window and an upload failure; same-generation caps 0→3
  loads the cursor and offscreen rows and window moves then use engine
  copies.
- `tests/host/desktop/accel_notice_test.c`, `tests/host/splash_progress_test.c`:
  the three texts, golden splash frames (`splash-accel-ok`, `-yes`
  unchanged, `-no`).

Window move model, 1024x768x32, Ciuki Blue shell, builtin wallpaper, one
640x480 window dragged 40 steps of (8,6):

| Presenter | CPU bytes to the LFB | At 134 MB/s (f4-20 T23 model) | Engine requests |
| --- | ---: | ---: | --- |
| Software | 48,326,304 (1.21 MB per step) | 360.6 ms, 9.02 ms per step | none |
| Engine | 2,079,880 (52 KB per step) | 15.5 ms, 0.39 ms per step | 35 window copies, 71 cache copies, 107 fills, 11.48 Mpx moved inside VRAM |

The engine's own copy/fill time on the T23 is not modelled (no measured
rate); setup uploads (background and window, 1,093,632 pixels) happen between
frames, 16 rows per idle turn.

## Expected T23 records (next image)

```
[accel] savage offscreen first=768 end=2048 claim=1
[accel] summary accelerated=1 driver=savage reason=none used=0
[accel] savage write_test result=ok phase=done write=1 offscreen=1 wrote=<M> read=00000000 error=0 rows=768-2048
[accel] summary accelerated=1 driver=savage reason=none used=1
[console] [desktop] accel fills=<n> copies=<n> writes=<n> fallback=0 cache_rows=<n> frames=<n> lfb_bytes=<n> caps=3 vram_rows=1280
```

The desktop notice reads `2D acceleration: YES — Savage` once the engine has
drawn (within 3 s of the decision), the splash (if still visible)
`2D engine: OK (drawing: software)`.

Reading a failure:

- `write_test result=failed phase=write … read=<low byte repeated>`: the host
  image write differs on silicon; caches stay off, fills and copies remain.
- `write_test result=ok phase=offscreen … offscreen=0`: offscreen rows are not
  written or copied back; caches stay off.
- `[accel] savage quarantined reason=<r> phase=draw` with `summary
  accelerated=0 … used=1`: a drawing request failed (`idle_timeout`,
  `event_timeout`, `fifo_timeout`, `dead_mmio`); the desktop shows
  `2D engine: NO (<r>)` and draws in software.
- `[desktop] accel … copies=0` while dragging windows with `caps=3`: the move
  copies were refused by the desktop (obscured or off-screen windows), not
  by the engine.

Visual checks: drag windows over the wallpaper and over each other, open and
close the Start menu, raise windows, change the wallpaper and select desktop
icons (the cache follows), then Fn+F7 / suspend if available (firmware and
SMI code must not have changed offscreen memory: no stale wallpaper pieces).
