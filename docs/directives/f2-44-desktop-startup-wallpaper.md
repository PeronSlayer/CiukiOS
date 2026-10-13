# f2-44 — Desktop start-up: the wallpaper must never block the desktop

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-44` from `main` (f9f112a0). Host-testable. Files: `apps/desktop/desktop.c`, `wallpaper.c`, `image.c`, `settings*.c`, `shell.h`, `scripts/build_image.py`, `scripts/build_desktop*`, `tests/host/desktop/`. Do not edit `compositor.c` (f2-45) or the tray code of `shell.c` (f6-02).

## Evidence (T23, image 3299a24f, 2026-10-13)

The owner waits close to a minute after power-on before the desktop reacts; the first statistics window shows `input_lag_us=11047000`. The default wallpaper `/System/wallpapers/Ciuk2.png` is 2.56 MB (1448x1086); `wallpaper_load` (`desktop.c:440`) reads it, decodes it with stb_image and rescales it (`desk_image_scale`, mode fill) synchronously in the desktop loop, two iterations after the first frame (`wallpaper_deferred`). Disk reads run at 800 KB/s today (f1-39 fixes the disk; this directive must make the desktop independent of it). The desktop ELF is 1.74 MB.

## Task

1. **Measure:** verbose-boot records `[desktop] stage=<name> us=<since process start>` for: entry, settings read, fonts/icons ready, first frame presented, wallpaper file read, decoded, scaled, applied, first input handled. The lead reads them in the T23 capture.
2. **Never block:** the desktop presents the solid background and handles input immediately; the image wallpaper arrives later without a visible stall (> 50 ms between input and reaction is a stall). Choose and justify the mechanism from what the native API offers (check `docs/design/f2-*`, `src/kernel/proc`, the SDK headers): a worker thread if threads exist; otherwise a helper process (`/Programs/wallpaper` spawned by the desktop with the path, size and mode) that writes the rendered frame to a cache file and notifies the desktop (`want_reload` or a signal), the desktop reading the frame with one read.
3. **Cache:** rendered frames in the settings volume, `/System/cache/wallpaper/<sha8 of path+size+mtime>-<w>x<h>-<mode>.raw` (xRGB8888 rows, so a 1024x768 frame is 3 MiB), invalidated when the file, mode, colour or resolution changes; bounded (keep the last 3). Build-time pre-rendering in `scripts/build_image.py` of the default wallpaper for 800x600, 1024x768, 1280x800, 1280x1024 and 1400x1050 into the same cache namespace, so the first boot is fast too (hashes in the build manifest; the inventory tests of `tests/host/desktop/test_desktop.py` updated).
4. Decoder cost: measure stb_image + `desk_image_scale` for Ciuk2 on the host with `-O2` and report; if the scaler is the heavier part, replace it with an integer area-average with per-column precomputed offsets (exact same output for the host tests' golden hashes, or new goldens with the diff explained).

## Acceptance

Host tests: cache key/invalidation/bounds, the helper or worker path, the fallback when the cache dir is missing (read-only volume), and `desk_image_scale` goldens; the desktop host suite (`tests/host/desktop/`) green; `scripts/build_image.py` output verified by `test_desktop.py`. The lead checks on the T23 that the Start menu opens within 1 s of the first frame while the wallpaper is still loading.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-44-report.md` with the measured decode/scale times.
