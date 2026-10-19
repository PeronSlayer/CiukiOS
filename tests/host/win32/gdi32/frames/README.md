Pinned host reference frames for f5-10B. Each `.bin` is the complete padded
buffer described by `manifest.json`, including unchanged padding and X bits.
16-bit output is little-endian RGB565; 32-bit output is B,G,R,X. These files
were produced by CiukiOS's engine, not captured from Windows.

`tests/host/win32/test_gdi32.py` compares the engine output with independent
Boolean/conversion/stretch/region calculations, then with the pinned SHA-256
and bytes. The ten FreeType text frames also pin the font's SHA-256 and consume
the SDK's pinned v35 rasteriser. The manifest covers all 256 ROP3 truth tables
at both destination depths; six source depths with both 555 and 565 and both
orientations; four stretch modes at 17×11→9×7, 25×17 and 34×22; five region
operations; palette realisation and reserved-entry animation; text alignment.

Normal checks:

```sh
nice -n 19 python3 -B -m unittest tests.host.win32.test_gdi32
```

For an intentionally reviewed raster change, `CIUKI_GDI_PIN_FRAMES=1` permits
the same command to update the pins after checking independent oracles.
The executable capture recipe is `../engine_test.c`, function `frames`.
