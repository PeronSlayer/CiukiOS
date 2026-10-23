# Cirrus GD54xx synchronous 2D port

The f2-116 port uses the Linux **v6.12** donor, not the X.Org driver.
Primary sources checked before implementation:

- [Linux cirrusfb.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/cirrusfb.c),
  `cirrusfb_set_blitter`, `cirrusfb_BitBLT`, `cirrusfb_RectFill` and
  `cirrusfb_sync`: byte widths, minus-one dimensions, reverse byte addresses,
  equal foreground/background pattern expansion, ROP 0x0d, start 0x02.
- [Linux cirrus.h](https://github.com/torvalds/linux/blob/v6.12/include/video/cirrus.h):
  register definitions, retained unchanged with the original notice.
- [QEMU v9.2.0 cirrus_vga.c](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/cirrus_vga.c),
  `cirrus_bitblt_start`, `cirrus_bitblt_reset`, `cirrus_write_bitblt`,
  `cirrus_hook_write_gr`: GD5446 byte-addressed copy/expansion, start/busy bits,
  reset falling edge, 13-bit dimensions/pitch and 22-bit addresses.
- Cirrus Logic **CL-GD5446 Technical Reference Manual**, November 1996,
  [GR31, section 5.10](https://bitsavers.org/components/cirrusLogic/_dataSheets/385446-002_CL-GD5446_Technical_Reference_199611.pdf)
  ([searchable mirror](https://vgamuseum.info/index.php/news/item/download/230_fa3570576b579319d3094a42814695bc)):
  bit 0 busy, bit 1 start, bit 2 reset, bit 4 buffered registers, bit 7 autostart.
  The web index exposed this register page; full PDF retrieval was unavailable.
- The owner's Windows 98 Resource Kit, chapter 24, `wrkc24_3.htm`,
  "Display Driver Overview": the DIB engine handles operations the minidriver
  does not accelerate. This is the existing part A dispatch policy.

## Translation and safety decisions

Only the contract PCI pairs 1013:00a0/00a4/00ac/00b8 nominate this backend.
The existing registry proves sole display ownership, BAR0, complete active
framebuffer lease and firmware VRAM extent before activation. The adapter
also requires enabled VGA I/O decode and unlocked extensions. There is no
PCI configuration write, mode/PLL change, DMA or bus mastering. Modes stay VBE.

The common older subset is deliberately limited to 16/24 bpp, an extent of
2 MiB, pitch <=4095, byte width <=2048 and height <=1024. GD5446 additionally
accepts 32 bpp, 4 MiB, pitch <=8191, byte width/height <=8192. An unrepresentable
call refuses before register writes. COPY uses byte transfers, including the
last byte of the last row for reverse copies. FILL uses the donor's equal
foreground/background 8x8 expansion; it needs no reserved pattern allocation.
The eight source bytes at framebuffer offset zero are within the validated
lease. No host-data transfer or engine-only allocation is advertised.

The Linux unbounded waits are replaced by a 20 ms deadline, a frozen-clock
iteration cap and the part A presenter pause hook at 1 ms boundaries. Probe
is read-only PCI nomination. Engine initialization and saved-pixel FILL/COPY
qualification run only through `fbdev_accel_probe` under drawing exclusion.
Every command is synchronous. A fault disables submissions for the boot.
Reset uses the donor's 0x04/0x00 sequence; CPU safety requires two subsequent
idle status reads, never the reset write or quarantine flag alone. Failed
proof retains a rejecting guard and the port lease. COPY errors are returned
without replay; the generic caller reconstructs its pixels.
Register access also requires both leases to remain ACTIVE, not merely to
retain their generations. Port release follows the registry's quiesce/proof/
release protocol; a failed port activation is quarantined before any I/O.

The donor has no verified cursor callback or native WRITE. Those optional
hooks and capability bits remain absent; cursor requests return EOPNOTSUPP
through part A, with the desktop retaining cursor ownership. MONO and the
other part C operations have no part A signatures and remain for f2-101d.
The generic DM9 native-only framebuffer comparison qualifies advertised
FILL and both COPY overlap directions; it never counts software replay.

Sources downloaded through the GitHub connector are confined to
`build/research/f2-116/`; emulator code is a reference and is not imported.
