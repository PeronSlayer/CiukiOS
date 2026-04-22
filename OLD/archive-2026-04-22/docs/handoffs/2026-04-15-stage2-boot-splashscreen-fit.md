# HANDOFF - stage2 boot splashscreen integration (ASCII fit-to-screen)

## Context
User provided boot splash ASCII at `misc/splashscreen.txt` and asked to show it at CiukiOS startup.

## What Changed
1. Added a dedicated splash module:
   - `stage2/include/splash.h`
   - `stage2/src/splash.c`
2. Added automatic build-time conversion from text file to C array:
   - `misc/splashscreen.txt` -> `build/generated/splash_data.c` via `xxd -i`
   - generated symbol names: `stage2_splash_ascii`, `stage2_splash_ascii_len`
3. Hooked splash rendering into boot flow (`stage2_main`):
   - render after title bar
   - wait up to ~2s (or skip on keypress)
   - clear text area and continue to shell
4. Added video API to know text-space height:
   - `video_text_rows()` in `video.h`/`video.c`
5. Updated boot test markers to validate splash render event.
6. Added `xxd` to run script dependency checks.

## Rendering Behavior
- Splash is parsed as line-based ASCII.
- It is downsampled to current text viewport (`columns` x `text_rows`) preserving the whole image.
- Non-printable chars are replaced with spaces.
- Rightmost screen column is intentionally unused to avoid line-wrap side effects.

## Source Size Detected
Serial marker shows source dimensions in hex:
- `src=0x190 x 0xDC` => `400 x 220`

Note: user said 400x400, but current file is 400 columns and 220 rows.

## Files Modified (this step)
1. `Makefile`
2. `run_ciukios.sh`
3. `scripts/test_stage2_boot.sh`
4. `stage2/include/video.h`
5. `stage2/src/video.c`
6. `stage2/include/splash.h` (new)
7. `stage2/src/splash.c` (new)
8. `stage2/src/stage2.c`

## Validation
Executed:
1. `make test-stage2` -> PASS
2. `make test-fallback` -> PASS

Relevant marker found:
- `[ ok ] splashscreen rendered src=0x...`

## Risks / Limits
1. Splash is static at build-time (rebuild required after editing `misc/splashscreen.txt`).
2. Current downsampling is nearest-neighbor in text space.
3. Splash duration is fixed (~2s) unless a key is pressed.

## Suggested Next Steps
1. Add shell command `splash` to re-render splash on demand.
2. Add optional config for splash timeout (e.g. `CIUKIOS_SPLASH_TICKS`).
3. If desired, support 1:1 centered crop mode as alternative to full-fit scaling.
