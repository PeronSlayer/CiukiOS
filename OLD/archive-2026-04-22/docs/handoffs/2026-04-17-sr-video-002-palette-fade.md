# Handoff — SR-VIDEO-002 DOOM-prep palette fade + mode13 bulk fills

**Date:** 2026-04-17
**Scope:** Palette fade helper + mode13 bulk fills + `FADEDMO.COM` sample (DOOM-style screen fades).
**Version bump:** `CiukiOS Alpha v0.8.1` → `CiukiOS Alpha v0.8.2` (continuing 0.8.x per user directive).
**Branch:** `feature/copilot-sr-edit-001`

## Context and goal
Extends v0.8.1's VGA-mode compatibility surface with the two primitives DOOM's video layer actually exercises every frame: (a) palette fades (blood flash, intermission, title transitions), (b) rectangular palette-indexed fills on the 320×200 plane. Keeps the work inside the 0.8.x line.

## Files touched

### New files
- `com/fadedemo/fadedemo.c`
- `com/fadedemo/linker.ld`
- `docs/handoffs/2026-04-17-sr-video-002-palette-fade.md` (this file)

### Modified files
- `stage2/include/gfx_modes.h` — declared `gfx_palette_fade`, `gfx_mode13_fill`, `gfx_mode13_fill_rect`.
- `stage2/src/gfx_modes.c` — implemented the three new APIs plus a captured-baseline mechanism for fades; external `gfx_palette_set` now invalidates that baseline.
- `boot/proto/services.h` — extended `ciuki_gfx_services_t` with `palette_fade`, `mode13_fill`, `mode13_fill_rect` (append-only before `reserved[32]`).
- `stage2/src/shell.c` — added the new function pointers to `g_gfx_services`.
- `Makefile` — added `COM_FADEDEMO_*` vars + build rule + `all` entry.
- `run_ciukios.sh` — copy `FADEDMO.COM` into the FAT image.
- `stage2/include/version.h`, `documentation.md`, `CLAUDE.md`, `README.md`, `CHANGELOG.md` — v0.8.2 bump.

## Decisions made
1. **Captured-baseline fade model.** The first call with `step=0` snapshots the live palette into `g_palette_fade_base`; each subsequent step linearly interpolates `base → target` by `step/total`. At `step=total` the baseline is invalidated so the next fade cycle recaptures. Matches the DOOM `I_SetPalette`-style usage pattern.
2. **External `palette_set` invalidates the baseline.** If a caller mutates the palette during a fade, the next `fade(step=0,...)` re-captures rather than interpolating from a stale snapshot.
3. **`gfx_mode13_fill` aliases `gfx_mode13_clear`.** Same semantics, but names the DOS idiom callers expect (`mode13_fill(color)` reads better than `clear(color)` when color isn't 0).
4. **`mode13_fill_rect` mirrors the 32bpp `video_fill_rect` API shape** (`x,y,w,h,color`) so ABI feels consistent across planes.
5. **Forward-declared fade state above `gfx_palette_set`.** Needed because the set path has to touch the baseline-valid flag; avoids `extern` on a file-static.
6. **No stage2 shell command exposed.** The three APIs are primarily ABI consumers for DOS programs; `FADEDMO.COM` is the validation vehicle. Keeping the shell surface minimal.
7. **Staying on 0.8.x.** Per user directive, no v0.9.0 bump. Cadence OK: bump every 3-4 tasks (v0.8.0 → v0.8.1 after M-V2.4+M-V2.5; v0.8.1 → v0.8.2 after palette-fade + mode13-fill + FADEDMO sample).

## Validation performed
1. `make all` — clean build, zero warnings, zero errors.
2. Artifacts:
   - `build/FADEDMO.COM` (816 B), `build/DOSMD13.COM` (752 B), `build/stage2.elf` (2.6 MiB).
3. **Pending user validation (in QEMU):**
   - `run FADEDMO.COM` displays concentric bands, fades to red, then to black, prints `[fadedmo] OK`, returns.
   - `run DOSMD13.COM` and `run GFXSMK.COM` still work (regression).

## Risks and mitigations
1. **Busy-delay inside FADEDMO.** Each fade step commits an ~8 MiB upscale; the `busy_delay(2000000)` is just additional pacing. On very fast hosts the fade may still be perceptibly quick; acceptable for a smoke demo.
2. **Baseline capture latency.** Snapshotting 256 u32s on step=0 is trivially cheap; no concerns.
3. **ABI growth.** Three new function pointers appended before the unchanged `reserved[32]`. Consumers compiled against older headers read up to the old set; never dereference the new slots. Backwards-compatible.
4. **Fade precision.** Division is integer; at 16 steps the rounding error is ≤1 per channel, invisible in a fade. For finer fades pass a larger `total` (e.g. 64 or 256).

## Next step
- Optional: additional DOOM-prep helpers (`gfx_mode13_blit_indexed`, column fill for R_DrawColumn fast path) → still 0.8.x.
- M-V2.6 desktop WM polish remains optional → could land as v0.8.3 if desired.
- Real DOOM port integration enters the v0.9.x cycle whenever the user decides to leave 0.8.x.

## References
- Subroadmap: `docs/subroadmap-sr-video-002.md`
- Previous handoff: `docs/handoffs/2026-04-17-sr-video-002-milestones-v24-v25.md`
