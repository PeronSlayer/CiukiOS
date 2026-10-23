# NVIDIA Riva 128 / NV3

Independent native port of audited X.Org xf86-video-nv fragments at commit
`0b6619fec39fada995646fb95bb25e72d3c9a491`; canonical project:
https://gitlab.freedesktop.org/xorg/driver/xf86-video-nv . The preserved-history
mirror and every inspected source hash are in `import.json`. Review copies
were fetched only into this worktree's ignored `build/research/riva128/`.

| Local file | Donor closure / adaptation |
| --- | --- |
| `engine.c` | `src/riva_hw.c` (`nv3Busy`, configuration/VRAM decode, engine-only part of `LoadStateExt`), `src/riva_hw.h` object register layout, `src/riva_xaa.c` fill/copy/pattern/reset sequences; bounded waits and errors replace XAA macros. Cursor conversion derives from `src/riva_cursor.c`, with its original notice in `cursor-notice.h`. |
| `tables.h` | `src/riva_tbl.h`: RivaTableFIFO, nv3TablePFIFO/PGRAPH/PRAMIN and 15/32-bit PRAMIN contexts; original DWORD-indexed values retained. Interrupt enables are forced off by the loader. Local VRAM context limit/page entries are restricted to the detected VRAM. No PMC-reset/PTIMER/mode tables imported. |
| `driver.c` | CiukiOS registry, keyed marker, ownership and mapping glue. BAR mapping and 32 KiB reservation follow `src/riva_setup.c` and `src/riva_driver.c`. Native-only activation comparisons use `swdib` only for the RAM reference. |
| `nv3.h` | Private host/test boundary, with no public ABI changes. |
| `nvidia-notice.h`, `cursor-notice.h` | Original NVIDIA and David J. McKay notices and Jarno Paananen credit. |

The installed combined notice is `docs/licenses/riva128-NOTICES.txt`.
No source includes another `.c` file. No complete X server, Linux or Haiku
compatibility layer, mode setter, DMA pushbuffer or 3D code is imported.

Only PCI `12d2:0018` matches. FILL/COPY/SYNC, legacy PATTERN and the 32x32
opaque-mask/foreground-select cursor have part A hooks. RGB555/RGB565 in
16-bit words and XRGB8888 in 32-bit words qualify; packed 24-bit modes,
nonzero scanout offset/pan, unsupported pitch or channel layouts use software.
Cursor coordinates must fit signed 16-bit hardware fields; other coordinates
refuse before mutation. No new optional capability numbers are invented.
MONO/INVERT/LINE await the part C interfaces. VBE owns mode setting.

Transport, source links, DM9, idle proof and release rules are recorded in
[`docs/design/riva128-2d.md`](../../../../../docs/design/riva128-2d.md).
Run `nice -n 19 python3 -B tests/host/test_riva128.py` from the repository root.
It runs UBSan and the actual i686 kernel compiler flags. Set
`CIUKI_DISPLAY_ASAN=1` to add ASan with its default leak detection. Host fake
registers prove command packing and software agreement, not physical silicon
behavior or the adequacy of undocumented donor constants. Physical NV3
qualification remains the lead's task; no emulator models this backend.
