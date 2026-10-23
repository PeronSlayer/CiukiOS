# Mach64 native 2D port (f2-114)

The port uses Haiku **r1beta5**, commit
`a1f00118db8b858feb73f64ac4dc3d038b82bf9b`, with native CiukiOS glue.
Primary sources inspected before implementation:

| Source | Decision |
| --- | --- |
| [Haiku ati/mach64_draw.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_draw.cpp) | Preserve pixel widths, chain masks, FIFO groups, solid source, copy ROP and both copy directions. Convert inclusive donor dimensions to CiukiOS lengths. |
| [Haiku ati/mach64_init.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_init.cpp) | Replace infinite FIFO/GUI loops with deadlines, iteration limits and presenter pauses. Read memory size; never import DSP, PLL or panel setup. |
| [Haiku ati/mach64.h](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64.h) | BAR2-relative register offsets include the 0xc00 block-zero displacement. Import only used definitions with the original notice. |
| [Haiku ati/mach64_cursor.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_cursor.cpp) | Translate legacy opaque/image planes into packed two-bit cursor pixels, including little-endian pair order, transparent padding and negative positions. Claim a separate VRAM page before upload. |
| [Haiku kernel ati/driver.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/ati/driver.cpp) | BAR0 is VRAM, BAR2 is MMIO; validate CONFIG_CHIP_ID. Use only the contract's nine explicit Rage Pro/Mobility IDs. Do not import PCI command writes, bus mastering, ROM decoding or the unbounded alias probe. |
| [Linux v6.12 include/video/mach64.h](https://github.com/torvalds/linux/blob/v6.12/include/video/mach64.h) and [aty/atyfb_base.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/atyfb_base.c) | Cross-check pixel-width/CRTC fields, FIFO error bit and PCI IDs. These are references, not imported code. |
| [Haiku ati/mach64_util.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_util.cpp), same ref | Ratio helpers concern modes; no import needed for FILL/COPY/SYNC. |
| Owner's read-only Windows 98 Resource Kit, wrkc24_3.htm, Display Driver Overview; wrkc28_8.htm, Graphics Subsystem | Unaccelerated drawing uses the common DIB engine. Retain part A's refusal and CPU-idle safety distinction. No book text is copied. |

The existing part A registry proves the active BAR0 framebuffer lease and
rejects ambiguous adapters. The driver additionally validates BAR2, excludes
RAM/kernel aliases and framebuffer overlap, compares firmware CRTC pitch,
offset and pixel width with the active framebuffer, and bounds memory with
both firmware VRAM and MEM_CNTL. Probe itself performs no MMIO or claims.
24-bit packed pixels and unusual channel layouts are refused before commands.
Only RGB565 and XRGB8888 use the donor's 16/32-bit paths.

Engine configuration and saved-pixel activation run under fbdev_accel_probe's
exclusive presenter/drawing claim. Quarantine latches for the boot, removes
capabilities, hides the cursor and never alone grants CPU access. Fallback
requires an empty FIFO, idle GUI and verified cursor disable. An unproved
stop retains the rejecting guard. COPY errors after launch are not replayed.
No DMA, host transfer, mode, clock, DSP, LCD or scanout register is written.

Part A has no INVERT hook or capability allocation. The donor's INVERT path
therefore waits for f2-101d; unsupported pattern/write operations use swdib.
Legacy cursor hooks and bits are retained without public ABI additions.
The shared conservative accel.cfg marker blocks on any nonempty prior record,
including the legacy marker. Only this boot's committed attempt may be cleared.

Drivers without a configuration-sweep hook use the generic DM9 diagnostic
directly, in the firmware's current mode. This avoids the older sweep's
16-bpp mode transition on the E500. The display name reaches inventory and
Control Panel through part A's existing generic records.

Host fake-register traces and swdib comparisons are model evidence only.
The lead must qualify the E500's actual PCI ID, modes and cursor on hardware.
QEMU ati-vga does not model this chip. No image or hardware result is claimed.
