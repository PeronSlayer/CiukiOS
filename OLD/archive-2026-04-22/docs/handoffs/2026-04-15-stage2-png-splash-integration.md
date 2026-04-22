# Handoff - PNG Splash Integration (2026-04-15)

## Objective
Integrate a real graphic splash image (`CiukiOS_SplashScreen.png`) into Stage2 boot path with build-time conversion and runtime fallback safety.

## What was implemented

1. Build pipeline now converts PNG to C asset at build time.
2. Stage2 graphic splash renderer now uses embedded RGBA image first.
3. Existing ASCII-based graphic fallback remains active if image asset is unavailable/invalid.

## Files changed

- `Makefile`
  - Added splash image asset targets:
    - `SPLASH_IMAGE_SRC` (default `misc/CiukiOS_SplashScreen.png`)
    - generated C: `build/generated/splash_image_data.c`
    - object: `build/obj/stage2/splash_image_data.o`
  - Added automatic fallback stub generation if image is missing.

- `scripts/generate_splash_image_c.sh` (new)
  - Converts input image to RGBA8888 raw bytes.
  - Resizes with cap (`--max-dim`, default 768) to keep binary size reasonable.
  - Emits C source with:
    - `stage2_splash_image_width`
    - `stage2_splash_image_height`
    - `stage2_splash_image_rgba[]`
    - `stage2_splash_image_rgba_len`

- `stage2/src/splash.c`
  - Added embedded image symbols and validation path.
  - `stage2_splash_show_graphic()` now:
    - fills background black,
    - renders RGBA image centered/scaled with aspect ratio,
    - alpha-blends over black when alpha < 255,
    - falls back to previous ASCII-luma renderer if image is not available.
  - `stage2_splash_source_cols/rows()` now report image dimensions when image asset is present.

- `misc/CiukiOS_SplashScreen.png` (new)
  - Imported source image from user-provided asset.

## Runtime behavior summary

1. On boot, Stage2 splash uses real image renderer.
2. If image asset is missing or invalid, Stage2 automatically falls back to ASCII-based renderer.
3. Shell preview command (`gsplash`/`splash`) uses the same path.

## Validation

All regression tests pass after integration:

- `make -j4`
- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`

## Notes for next iteration

1. Optional: add nearest/bilinear toggle for scaling quality.
2. Optional: add palette quantization path for 16-bit target tuning.
3. Optional: support selecting splash image via env/make var for theme packs.
