# Wallpaper verification — 2026-09-26

The packaged native wallpaper feature passed actual QEMU control/pixel checks
in 800x600x8 and 1280x1024x32, followed by cold restarts. The test copied the HDD;
no guest binary, asset, configuration file or RAM state was injected. Video
profiles were selected through the real `VGASETUP desktop` preview and confirmed
with Enter. This is emulator evidence, not physical T23/other GPU qualification.

- QEMU: QEMU emulator version 11.1.0; configured Pentium III and 128 MiB RAM, KVM acceleration.
- Source image SHA-256: `ad1cc11ba7e1f3506f0e321f6eda5415d515f96a393781080a07dff5c50421fa`.
- Packaged SHELL.COM SHA-256: `77a091c4ccad79bfcace4b8fe3c443756d283cd03018b4b1aa2614371f1c2223`.
- Wallpaper source SHA-256: `669fa5337d58ff5e9dfbadbe97f4af09f1dc0ad68c3141b971a69451d0a86798`.
- The source HDD image remained unchanged; all test VMs were closed.

## Actual guest sequence

For each color depth, the harness opened Programs > System > Wallpaper through
keyboard/mouse input, applied the 256x256 tile (65,536 source bytes), then a
different 160x160 tile. It checked the persisted `WALL.CFG`, dragged and closed
a real Run window, entered DOS, executed COMDEMO, returned to the desktop, then
started a fresh QEMU process with the same HDD copy. Each stage compared actual
uncovered screenshot pixels to the original BMP or its exact indexed palette
mapping; existing chrome, shortcuts and visible windows were excluded explicitly.

| Mode | Compared pixels per stage | Stages | Different pixels | Tolerance |
| --- | ---: | ---: | ---: | ---: |
| 800x600x8 | 195,528–304,800 | 6 | 0 | 0 |
| 1280x1024x32 | 898,688–1,007,960 | 6 | 0 | 0 |

The 8-bit oracle uses QEMU's actual 6-to-8-bit DAC expansion, including its
low-bit replication, rather than an approximate scaling tolerance. Source:
[QEMU vga_int.h, c6_to_8](https://gitlab.com/qemu-project/qemu/-/blob/master/hw/display/vga_int.h).
An initial smoke used a three-value DAC rounding tolerance; the final 8-bit
run above supersedes it with an exact comparison. A preliminary harness
assumption about the shipped default profile was corrected before exercising
the wallpaper; no production change was needed.

All 11 packaged wallpaper files were separately decoded and compared to their
original BMPs. Every source pixel, palette color and native dimension matched.

## CPU-level coverage for additional formats

`test_wallpaper_pixels.py` assembled the production include and executed its
actual NASM instructions under Unicorn. It verified all 256 colors for 15-bit
RGB555, 16-bit RGB565, 24-bit RGB888, 32-bit RGB888 and reversed-channel 32-bit
metadata. Twenty band cases covered a source offset crossing 0xFFFF, repeated
tiles, nonzero damage origins and zero-size rectangles. Output bytes and guard
memory matched exactly; general registers, segments and IF were preserved.
Painting invoked no interrupt. This complements the actual guest checks and is
not presented as a BIOS or physical-GPU test.

## Reproduction and artifacts

```sh
python3 scripts/qemu_test_wallpaper.py --mode 8 \
  --image build/full/desktop-platform-2026-09-26/ciukios.img \
  --output build/full/desktop-platform-2026-09-26/wallpaper-8-exact
python3 scripts/qemu_test_wallpaper.py --mode 32 \
  --image build/full/desktop-platform-2026-09-26/ciukios.img \
  --output build/full/desktop-platform-2026-09-26/wallpaper-32
uv run --with unicorn python scripts/test_wallpaper_pixels.py \
  --output build/full/desktop-platform-2026-09-26/wallpaper-cpu
```

Each output folder contains `result.json`; actual VM outputs also retain serial
logs, QEMU arguments and screenshots. `desktop.png` and `drag.png` are lossless
conversions of the final captured PPM images. These test outputs contain the
owner's optional Windows artwork and are not public wallpaper redistribution.

## Final packaged candidate recheck

After the filesystem/input repairs and the change to release font/logo memory
before DOS execution, the complete 32-bit sequence was repeated on the final
packaged candidate. All six stages passed, comparing 898,688–1,007,960 pixels
per stage with **zero differing pixels and zero tolerance**, including return
from COMDEMO and cold restart. All eleven tiles still matched their source
BMPs exactly. No binary, configuration or RAM overrides were used.

- Image SHA-256: `f2aa95a62bc4830d4c7560aec94524b868730b8a1c4c40ae618a547ea8eb001d`.
- SHELL.COM SHA-256: `9454f6d6d03a095ef6a3bd3116d3db468e1ab953579128242e4977054e2928bd`.
- Report: `build/full/desktop-platform-2026-09-26/final-wallpaper-32/result.json`.
- Scope: QEMU, Pentium III/128 MiB, 1280x1024x32. The 8-bit evidence above
  belongs to the earlier recorded image. No physical GPU or native 2K claim
  follows from these checks.

```sh
python3 scripts/qemu_test_wallpaper.py --mode 32 \
  --image build/full/desktop-platform-2026-09-26/ciukios-candidate.img \
  --output build/full/desktop-platform-2026-09-26/final-wallpaper-32
```
