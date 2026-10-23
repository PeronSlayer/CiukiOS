# VMware SVGA II synchronous FIFO backend

Implementation record for f2-111, 2026-10-23. This supplements the display
driver model, Amendment 1, and the lead checkout's video-driver-families
sections 4 (VMware and VMSVGA), 5, 6 and 10. Those family and command-semantics
documents are absent from the directive's part A base; they were read from
the lead checkout without modifying it.

## Primary sources and repository comparison

- [X.Org svga_reg.h, commit 8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-vmware/blob/8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89/src/svga_reg.h):
  MIT protocol header, imported unchanged apart from an origin comment.
  FIFO offsets are bytes, one word must remain free, commands may wrap, and
  SYNC followed by BUSY=0 proves a drain. Optional registers require both
  sufficient register space and the corresponding capability. FRONT_ROP_FILL
  is a hint about an already filled GFB, not a native framebuffer fill.
- [QEMU v9.2.0 hw/display/vmware_vga.c](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/vmware_vga.c):
  behavior reference only. Legacy RECT_FILL opcode 2 and capability bit 0
  modify VRAM; RECT_COPY handles overlap using ordered rows and memmove.
  The model exposes neither fences nor alpha cursor capabilities. Basic FIFO
  MIN is 16 bytes; the data region must have at least 10 KiB. CONFIG_DONE=0
  alone does not restart its dirty log; reasserting the unchanged ENABLE=1
  does. Its native mode is 32 bpp, whereas other protocol implementations
  may support 16/24 bpp. Never change the firmware-selected geometry.
- [QEMU v9.2.0 ui/cursor.c](https://github.com/qemu/qemu/blob/v9.2.0/ui/cursor.c):
  mono mask bytes are MSB-left. DEFINE_CURSOR with a 1-bit AND plane and a
  32-bit XOR plane represents the legacy opaque/background/foreground cursor.
  Ciuki's opaque mask is inverted and each mask word is byte-swapped for
  the wire; packed framebuffer colours are used directly at 32 bpp.
- [VirtualBox DevVGA-SVGA.cpp, commit 74117a1cb257c00e2a92cf522e8e930bd1c4d64b](https://github.com/mirror/vbox/blob/74117a1cb257c00e2a92cf522e8e930bd1c4d64b/src/VBox/Devices/Graphics/DevVGA-SVGA.cpp):
  behavior reference only, no imported Oracle code. Its actual capability
  set includes COPY, extended FIFO, cursor bypass 3 and alpha cursor, but
  excludes legacy FILL and FIFO ACCELFRONT. It must refuse this native
  FILL/COPY binding; accepting its unadvertised legacy fill would violate
  the contract. Extended-FIFO fixtures also exercise a hypothetical device
  that explicitly advertises legacy FILL; they do not qualify VirtualBox.
- The read-only Windows 98 Resource Kit, `wrkc24_3.htm`, Display Driver
  Overview, describes the DIB engine handling operations that a minidriver
  cannot accelerate. The existing registry and swdib already implement that
  boundary; unsupported commands stay there.

## Implementation decisions

Only `15ad:0405` class 03 is nominated, with framebuffer BAR1, I/O BAR0 and
FIFO BAR2. `15ad:0710` is the earlier interface, not the selected SVGA II
BAR layout, and is refused. BARs, decode, unique display ownership, the
complete active fbdev lease, E820/kernel aliases and protocol readbacks must
agree. Probe does not access the engine. Start claims ports and a bounded UC
FIFO mapping, checks existing ID2, enabled scanout, matching geometry, pitch,
RGB masks and zero framebuffer offset. No PCI writes or bus mastering.

An already configured or busy foreign FIFO is refused without takeover.
The driver initializes an idle, disabled FIFO and enables it only while a
synchronous command owns the presenter. A complete packet is published with
one NEXT_CMD write after a write drain. A validated empty ring and BUSY=0
prove completion. FIFO disable plus unchanged ENABLE reassertion restore
automatic CPU updates before cpu_ready becomes true. This avoids adding a
waiting hook to the console or claiming CPU uploads as native WRITE.

All polls have a 20 ms monotonic budget, a fixed iteration cap, and 1 ms
presenter pauses when supplied. Quarantine disables submissions and hides the
cursor but does not prove idle. Fallback may make one bounded drain attempt;
failed proof retains rejecting ops and cpu_ready=false. Submitted COPY errors
are never reported as clean refusal. Claims/mappings remain pinned until a
successful stop proves idle and revokes CPU register access.

The separate durable attempt file is `C:/system/settings/vmsvga.cfg`; it is
committed and closed before activation, and only cleared after successful
initialization. A nonempty/unreadable record blocks activation and is never
silently cleared. The old driver's unkeyed marker is left untouched. Generic
keyed markers still belong to part B.

FILL/COPY/SYNC and the existing legacy 32x32 cursor hooks are implemented.
Cursor capability requires 32 bpp and an advertised bypass mechanism; SET
hides before replacement, stop and quarantine hide, and diagnostic evidence
checks coordinate/visibility registers separately from framebuffer pixels.
PATTERN/WRITE and new part C operations remain software; no alpha, scaler,
offscreen, DMA, 3D or FRONT_ROP_FILL claim. Native mode-provider programming
is deferred under section 5's explicit later-directive boundary; VBE remains
the mode setter. No public ABI or SDK records are changed.

The generic DM9 entry must also run for drivers without a family-specific
configuration sweep. Its native-only saved-block comparison checks fill and
both overlap directions and restores only after proven idle. Driver activation
uses the same saved-block principle under fbdev_accel_probe. Runtime evidence
is still required from the lead; host protocol models are not hardware proof.
