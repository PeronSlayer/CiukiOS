# Trident Blade / CyberBlade native 2D port

Directive: f2-113. The family and optional-command contracts are read from
the lead checkout because they are absent at this part A branch base.
This supplements the binding display-driver-model contract; VBE owns modes.

## Primary-source comparison and implementation decisions

- Linux **v6.12**, [drivers/video/fbdev/tridentfb.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/tridentfb.c),
  `is_blade`, `is_xp`, `blade_*`, `xp_*`, `trident_pci_probe`;
  [include/video/trident.h](https://github.com/torvalds/linux/blob/v6.12/include/video/trident.h)
  defines the register offsets and PCI IDs. Port only Blade and XP FILL/COPY
  and initialization, replacing the donor's unbounded wait/silent reset with
  error returns, deadlines and a frozen-clock iteration cap. BAR0 owns the
  framebuffer and BAR1 owns registers. Image/TGUI, E4 and XPm derivatives
  outside the directive's explicit IDs are refused.
- X.Org xf86-video-trident, commit
  **64c02f307d7146641adf65e31bcaa2d1238479e7**, [src/xp_accel.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-trident/blob/64c02f307d7146641adf65e31bcaa2d1238479e7/src/xp_accel.c),
  `XPInitializeAccelerator`, corroborates the XP byte-pitch shift. Linux's
  `pitch << (bpp >> 3)` switch differs from byte pitch for 32bpp. Use explicit
  byte-pitch encodings and test each; no implicit default for unknown pitches.
  This is a Ciuki correction, not an unchanged donor claim.
- The same pin's [src/blade_accel.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-trident/blob/64c02f307d7146641adf65e31bcaa2d1238479e7/src/blade_accel.c)
  distinguishes depth 15 from depth 16. Initially accept RGB565 and XRGB8888;
  RGB555, packed 24bpp and other layouts use software. The Linux donor has no
  hardware cursor or pattern hook. Its Blade-only host mono stream requires
  a new part C hook and bounded FIFO protocol; no host stream, optional bit
  or cursor capability is advertised in part A.
- [src/trident_dac.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-trident/blob/64c02f307d7146641adf65e31bcaa2d1238479e7/src/trident_dac.c)
  documents the Blade3D permanent PCI retry quirk. Submit one local-VRAM
  operation only after idle; never issue a host-data command, DMA, bus-master,
  PLL, panel or PCI decode write. Save and enable the donor's MMIO/engine
  access bits under exclusive activation ownership, verify them, and restore
  them after idle at stop. Leave firmware retry settings untouched. Software
  deadlines cannot preempt a PCI transaction that never returns; the physical
  transport and retry quirk require lead review and hardware confirmation.
- The owner's Windows 98 Resource Kit, `wrkc28_8.htm` (Graphics Subsystem)
  and `wrkc26_5.htm` (graphics compatibility), describe the universal DIB
  engine/minidriver split and independent acceleration/cursor disablement.
  Keep software fallback separate and allow CPU access only after idle proof.

## Safety and activation

The registry proves the unique active BAR0 framebuffer lease before the
read-only probe. Recheck that binding at start. Validate BAR1 alignment,
overflow and absence of RAM, kernel, framebuffer and peer-BAR aliases;
claim and UC-map only the 4 KiB page containing the engine registers.
Firmware scanout start and pitch must match the zero-origin VBE framebuffer.
Claims never enable bus mastering. No register writes occur during probe or
eligibility; activation changes only the MMIO and engine access bits, with
the donor's sequencer unlock/restore sequence and register read-back.

Activation uses `fbdev_accel_probe` for exclusive drawing ownership. Save a
16x16 block only after idle, initialize the engine, execute native fill and
both overlapping copy directions, compare each with swdib, and restore only
after idle. Success publishes FILL/COPY; any discrepancy quarantines for the
boot. A timeout disables submissions and CPU access. Fallback polls boundedly
for actual idle; no reset write counts as idle proof. Unsafe state retains
the guard, mappings, resource leases and attempt marker. Stop releases
resources only after idle and mapping revocation.

The per-driver durable attempt file is separate from the legacy driver's
marker. A nonempty/unreadable file blocks activation; BEGIN commits and
closes before hardware access, END truncates/commits only after validation.
No file I/O occurs under the presenter lock. Metadata flows through the
existing registry query and device inventory to Device Manager and Control
Panel; neither needs a chip-specific name table.

## PCI ownership

Register `1023:9880,8400,8420,8500,8520,8600,8620` as Blade and `1023:8820`
as XP. The shared `8400/8500` IDs belong to this Blade engine exactly once;
the VIA UniChrome backend must omit them. The family census requests this
decision without recording a prior resolution; integration must preserve it.
Do not infer an Image/TGUI command path from vendor 1023 alone.
