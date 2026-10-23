# 3dfx native 2D port (f2-117)

This port uses Haiku r1beta5, commit
`a1f00118db8b858feb73f64ac4dc3d038b82bf9b`, as selected by the family
contract. Sources were read through the connected GitHub source tool; shell
network DNS and the web fetch cache were unavailable. Downloaded sources stay
under `build/research/`.

| Primary source | Comparison and port decision |
| --- | --- |
| [Haiku 3dfx_draw.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_draw.cpp) | Preserve DST/SRC formats, fill ROP CC, GO launch and independent X/Y copy directions. Translate inclusive dimensions to Ciuki counts using count minus one for reversed endpoints. |
| [Haiku 3dfx.h](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx.h), [3dfx_mode.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_mode.cpp) | MMIO register reference, FIFO empty count and three consecutive not-busy observations after a 3D NOP fence. Replace infinite loops with a 20 ms deadline and 4001-poll limit; preserve 1 ms presenter slices. Import no timing/PLL or mode routines. |
| [Haiku 3dfx_cursor.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_cursor.cpp) | 1024-byte cursor, eight AND bytes then eight XOR bytes per row; location has a +63 bias. Translate legacy opaque mask to AND=~mask, XOR=image&mask, MSB left. Set hides first; unused pixels are transparent. Convert native packed palette to RGB888. |
| [Haiku 3dfx_init.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_init.cpp), [kernel driver.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/3dfx/driver.cpp) | BAR0 registers, BAR1 VRAM; read-only memory straps bound firmware VRAM. Keep VBE scanout at offset zero. Do not copy the donor's bus-master enable, broad mappings or cursor-at-zero relocation. Reserve a separate page beyond the entire framebuffer mapping. |
| [Linux v6.12 tdfx.h](https://github.com/torvalds/linux/blob/v6.12/include/video/tdfx.h), [tdfxfb.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/tdfxfb.c) | Corroborate register offsets, COMMAND_EXTRA clearing, endian bits in MISCINIT0, and cursor mode difference: Linux uses X11; Haiku's AND/XOR packing requires Microsoft mode. Reject incompatible scanout configuration. |
| [X.Org tdfxdefs.h](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/blob/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src/tdfxdefs.h) | Read-only VSA topology checks: refuse multifunction/snoop, enhanced multi-chip video, SLI LFB, AA LFB, SLI/AA engine state. No multi-chip reconfiguration. This file is a register reference only, not an imported implementation. |

The owner's read-only Windows 98 Resource Kit `wrkc24_3.htm`, Display Driver
Overview, was checked: unsupported on-screen operations go to the DIB engine.
Ciuki's `swdib.c` remains the fallback and golden reference. No extracted book
text is redistributed.

The registry proves one active display PCI owner, matching BAR1 and the exact
fbdev lease. The driver additionally rejects overlapping/unsafe MMIO ranges,
bad layouts, insufficient VRAM and VSA multi-chip state. Probe is read-only
and emits no engine command. First engine setup and saved-pixel comparison
run exclusively through `fbdev_accel_probe`, never outside drawing exclusion.
Firmware with PCIINIT0's PCI_STALL_ENABLE set is refused before any engine
write. The port never enables bus stalling or changes the PCI timeout policy;
each write first reserves FIFO capacity. This gate follows the PCIINIT0
bit definitions in the pinned X.Org register reference above.

Only the donor's RGB565 and XRGB8888 are enabled. FILL, COPY, SYNC and legacy
32x32 cursor hooks use part A's existing bits and records. PATTERN and WRITE
are absent from this donor. INVERT has no part A hook or bit; its registration
waits for f2-101d. No public ABI or SDK header changes are needed. Mode setting
stays VBE; there is no native mode provider, SLI, DMA, AGP, offscreen lease or
3D acceleration in this port.

A fault disables new drawing and cursor submissions. Quarantine does not
grant CPU safety. Recovery requires FIFO room for the ordered NOP fence,
three stable idle samples and read-back cursor disable before CPU fallback.
There is no reset and no automatic replay of failed COPY. Failed proof retains
the rejecting guard and MMIO lease. The nonwaiting `cpu_ready` cache gates
console access. Successful stop hides the cursor, proves idle and revokes
private mappings before releasing their leases. Durable, per-device markers
are committed and closed outside presenter locks; retained or unreadable
markers prevent activation. The existing Savage marker is untouched.

The generic DM9 test proves FILL and COPY in both directions with native-only
dispatch and saved-pixel restore after idle proof. The cursor check uses
base/palette/location/enable readback and always hides its diagnostic shape.
The H page now reaches that comparison for drivers without an optional
family sweep; the pre-existing family-sweep path is preserved.
Host tests model the command processor and compare its framebuffer to swdib;
they establish packing and ordering, not physical timing or silicon behavior.
Real Voodoo3 and single-chip VSA-100 each require separate hardware evidence.
