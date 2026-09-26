# Wallpaper repair and adding tiles

CiukiOS now keeps its wallpaper catalog in a separate DOS memory allocation,
supports 99 numbered tiles, and has a **Refresh** button in the native Wallpaper
panel. Files and Wallpaper stay in graphics mode throughout a copy and apply.

The previous public-build default generated an empty catalog; those builds had
no installed wallpaper to choose. Public builds now include three original CC0
patterns. Personal builds still include the owner's Windows BMP files only when
`CIUKIOS_PERSONAL_WALLPAPERS=1` is selected; this does not grant redistribution
rights to that artwork.

The reported wallpaper failure was not reproduced on the previously frozen
`final-r3` personal image: its existing 11 tiles applied, survived a DOS child,
and loaded after a cold QEMU restart. The old importer did have concrete limits:
11 slots were already full, only lowercase `*.bmp` files were discovered, and
there was no way to refresh the catalog after adding files.

## Add a wallpaper without rebuilding CiukiOS

Prepare PNG or BMP **tiles** on the host, for example:

```sh
python3 scripts/add_wallpapers.py \
  --image build/full/ciukios-full.img \
  --output build/my-wallpaper-pack \
  /path/to/Pattern.png
```

`--image` reads the FAT image to find the next unused consecutive tile number;
it does not modify the image. For an installed computer whose image is not
available, use `--after 11` if its last tile is `WALL11.CWP`. The pack contains
`WALL12.CWP` and instructions. Do not rename the numbered files or introduce
numbering gaps.

1. Place the generated pack on a supported medium and use **Files** to copy the
   new `WALLnn.CWP` files to `C:\SYSTEM\UI`. Existing files are not overwritten.
2. Open **Programs → System → Wallpaper**, click **Refresh**, select the imported
   tile (on subsequent pages when needed), then click **Apply**.

The catalog's existing names and order are preserved. Consecutive additional
files are discovered both on Refresh and at boot, so replacing `WALLS.DAT` or
rebuilding the OS is unnecessary. The selected tile is persisted in `WALL.CFG`.

The current renderer supports lossless indexed tiles of **1–256 pixels per
side**, with at most **256 distinct colors**. It repeats the tile at native
pixel size; it does not yet decode arbitrary photographs or implement
full-screen Fit/Fill/Stretch. Transparent images, larger dimensions, too many
colors and malformed inputs fail with an explicit conversion error. No input
is silently resized or quantized. PNG/BMP decoding happens in the host tool;
CiukiOS loads the validated CWP format. A malformed runtime tile reports
“Wallpaper could not be loaded.” and leaves the last applied tile intact.

## Validation and limits

- `scripts/test_wallpaper_import.py`: exact PNG/BMP round trips, mixed-case file
  extensions, 99-slot bounds, input-error atomicity, append-only pack behavior.
- `scripts/qemu_test_wallpaper_import.py`: actual native Files copy, Refresh,
  selection and persistence; a damaged tile must retain the last good wallpaper.
- `scripts/qemu_test_wallpaper.py`: actual VBE desktop pixels across apply,
  window damage, DOS child return and cold restart.
- `scripts/test_wallpaper_pixels.py`: instruction-level native color formats
  and clipping, complementary to QEMU integration.

QEMU evidence is not a physical T23/E500 qualification. Build artifacts under
`build/full/wallpaper-repair-2026-09-26/` retain the baseline and unsuccessful
candidate/harness runs; these are not all passing release evidence. In
particular the first candidate included an unrelated intermediate shell
history regression; use the final integrated image's validation report for
release status.

Focused candidate results:

- `build/full/wallpaper-repair-2026-09-26/import-test-r3/result.json`: PASS,
  304,800 exact desktop pixels after a rejected damaged tile and after a cold
  restart, native Files copy, saved choice and unchanged catalog verified.
- `build/full/wallpaper-repair-2026-09-26/latest-pixels-32/result.json`: PASS,
  1280 × 1024 at 32 bits, Apply/drag/window close/DOS child return/cold restart;
  every sampled pixel matches the lossless source tile.
- `build/full/wallpaper-repair-2026-09-26/conversion-validation.json` and
  `renderer-unit/result.json`: PASS. The latter executes the production
  renderer's instructions for 15/16/24/32-bit formats and clipped bands.

These focused candidates replace the shell on a private image copy. They are
not substitutes for testing the final fully packaged image with media and sound
modules installed.
