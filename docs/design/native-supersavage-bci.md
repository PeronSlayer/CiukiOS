# Native SuperSavage BCI backend

Research and implementation: 2026-10-07. Hardware allowlist: PCI `5333:8C2E`
only, SuperSavage IX/C SDR. `session_gpu_savage.c` is a freestanding integer C
implementation of the documented PIO command protocol. It changes engine
state while preserving the firmware's display timing and scanout state. It is
not an OpenGL implementation: its first 3D interface rasterizes an untextured,
Gouraud shaded, screen-space triangle with depth, texture, fog, blending,
stencil and culling disabled. The GPU performs rasterization and transfers.

## Research basis and decisions

Sources were consulted before implementing the backend. The pinned source
archives, hashes and licensing are recorded in
[the upstream research](savage-upstream-research.md).

- [X.Org xf86-video-savage 2.4.1](https://www.x.org/releases/individual/driver/xf86-video-savage-2.4.1.tar.xz):
  `src/savage_driver.c:2931` (`SavageMapMem`) selects BAR0 MMIO, BAR1 VRAM and
  BAR2 tiled apertures on SuperSavage; `src/savage_regs.h` gives a 512 KiB MMIO
  extent. `savage_driver.c:1834–1870` reads CR36 and the mobile RAM table.
  `src/savage_accel.c:1051–1117` uses the PM/SuperSavage descriptor convention,
  destination tiling value 1 and the 64-bit descriptor bit 0. The no-overflow
  PIO initialization at lines 158–190 enables BCI control bit 3 while keeping
  shadow-status DMA and the command overflow buffer disabled. `src/savage_xaa.c:450–590`
  supplies the descriptor order for native copy and fill commands.
- [Linux v6.2 Savage DRM](https://github.com/torvalds/linux/tree/v6.2/drivers/gpu/drm/savage):
  `savage_drv.h:233–246` defines the 32-dword on-chip FIFO, BCI window at
  `0x10000`, alternate status at `0x48C60` and non-Savage3D used-entry mask
  `0x001FFFFF`. `savage_bci.c:97–110` waits for enough FIFO space. The DRM
  dispatch and UAPI define triangle list command `0x80000000`, count in bits
  23–16 and skip mask `0xFA` for X,Y,Z,ARGB vertices. SuperSavage does not use
  command or vertex DMA; this backend submits PIO dwords only.
- [Mesa 7.11.2](https://archive.mesa3d.org/older-versions/7.x/7.11.2/MesaLib-7.11.2.tar.bz2):
  `savage_3d_reg.h` defines packed DrawLocalCtrl, DrawCtrl0/1, DestCtrl and
  destination write flush fields. `savagestate.c:1430–1505` establishes a
  nontextured pass-through color state. Destination control stores width in
  128-byte tiles and a 2 KiB aligned offset; it has no linear-destination
  choice. Therefore a BIOS linear framebuffer must not be presented as a
  tiled 3D surface.
- [Original S3 Savage4 register reference](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
  used only alongside the SuperSavage-specific upstream implementation:
  DrawLocalCtrl is MMIO `0x48584`, DestCtrl `0x485DC`, DrawCtrl0/1
  `0x485E0/4`, and destination write watermarks `0x485EC`. The BCI indices map
  to these registers as `0x4850C + index * 4`. The document describes RGB565
  and XRGB8888 destinations, 128-byte by 16-row tiles and 2 KiB offsets.

These protocol facts are implemented independently in CiukiOS. Original
upstream source and license notices remain in the downloaded research trees;
the production backend does not embed a Linux kernel driver or assume its
runtime, IRQ, DRM, AGP or memory services exist in DOS.

## Ownership and usable memory

### Preflight diagnostics (2026-10-08)

The T23 diskseq59 capture reaches the protected 1024x768x32 CPU framebuffer,
but the native driver reports stage 1 without any MMIO command or engine
ownership. Stage 1 previously combined geometry and PCI-resource predicates,
so that capture does not identify which predicate failed. A single visible
frame is sufficient for this gate: private scratch and its readback mapping
are separately bounded by usable VRAM after resource validation.

The read-only PCI checks follow the primary
[Linux v6.2 PCI register definitions](https://github.com/torvalds/linux/blob/v6.2/include/uapi/linux/pci_regs.h):
command bit 1 enables memory decode, class is bits 31:16 of offset 08h,
BAR bits 2:1 identify the memory-address type, bit 3 is prefetchability and
bits 31:4 are the base. Prefetchable 32-bit BARs remain accepted. The pinned
X.Org `SavageMapMem` selects BAR0 for SuperSavage MMIO, BAR1 for the linear
framebuffer and BAR2 for its tiled apertures. Diagnostic changes retain all
these predicates and perform no BAR sizing, PCI-command write or engine
command before they pass.

The fixed 128-byte status ABI retains stage in `error_stage & 0xff`.
For stage 1, reason bits above the low byte distinguish format, dimensions,
pitch, extent, missing PCI identity, class, memory decode, BAR0/1/2,
framebuffer mismatch and MMIO overflow. Input width/height/pitch/bpp are
recorded before validation. Discovered BAR bases are recorded before engine
ownership. `last_status` is a preflight detail while these reason bits are
present, rather than an alternate-status MMIO sample: it contains the mapped
byte length for format/geometry/pitch/extent failures, raw class/command/BAR value for
those failures, or requested physical base for a mismatch/missing device.
Successful engine status and completion counters retain their meanings.

The twelve reason flags occupy `0x000fff00`; their individual values are in
`session_gpu_savage.h`. The production backend passed 45 host protocol
scenarios, also under AddressSanitizer and UndefinedBehaviorSanitizer, and
compiled with the canonical OpenWatcom flags. These include the captured
1024x768, pitch4096, 3 MiB single-frame extent and 237x64 KiB usable-VRAM
limit with T23-shaped 32-bit BARs (prefetchable framebuffer/aperture). The
model qualifies all three native primitives without requiring a second front
page. It does not qualify physical T23 acceleration. Evidence is in
`build/tests/savage-preflight-20261008/report.json`.

CVSESSION must establish and verify a real BIOS LFB mode first. A banked mode,
including an unproven physical BAR alias, is never sufficient. Native bind
accepts the mapped front-buffer extent and a separate firmware-reported usable
VRAM limit, supplied from the VBE controller TotalMemory field. Old clients
which lack this limit retain the protected CPU transport.

The RAM amount decoded from CR36 is a physical limit, not proof of free VRAM.
The T23's recorded VBE controller exposes 237 × 64 KiB = 14.8125 MiB while
the device's CR36 encoding may describe 16 MiB. Native scratch is bounded by
the smaller firmware-usable amount, leaves an additional 64 KiB guard, starts
after all front-buffer pages owned by CVSESSION, and stays below the usable
extent. No scratch allocation enters the remaining BIOS-owned memory. BAR1
must exactly match the active BIOS physical LFB and PCI memory decode must
already be enabled. BAR0/1/2 are read without destructive BAR-size probes or
PCI command changes.

The backend maps only BAR0 MMIO and one private readback page. The private
tiled surface occupies VRAM after the owned front-buffer pages. Its row width
is rounded to 128 bytes and its height to 16 rows. BAR2 aperture registers,
stream registers, clocks, timings and scanout addresses are untouched.

Initialization saves every altered CRTC, descriptor and engine-state register.
Xorg's `SavageEnableMMIO` (`savage_driver.c:4175–4201`) establishes CR40 bit 0
before MMIO reads. The backend follows that order with temporary CRTC ownership
and restores the bit and locks on every failure before engine ownership begins.
The 64-bit descriptor is enabled with the PM/SuperSavage bit convention, all
write planes are enabled, and PIO is enabled with overflow and shadow DMA off.
Unsupported chips and invalid geometry are rejected before MMIO writes.

## Actual hardware proofs before capabilities

Native fill capability is advertised only after a private-VRAM BCI solid fill
has completed, the mapped readback pixels match the requested color and the
original readback data has been restored. RGB8888 checks its meaningful RGB
channels; RGB565 checks every pixel bit.

Native triangle capability additionally requires a separate hardware test:
the GPU fills a private tile with a background color, rasterizes a known red
triangle through `DRAW_PRIM`, and copies the tile through the 2D engine into
the private linear readback page. The backend verifies an interior red pixel
and an exterior background pixel. It restores that page only after proving
that both engines and the FIFO are idle. A cleanly drained triangle mismatch
keeps native fill available and leaves triangle capability absent. A timeout
removes both capabilities and retains ownership.

The 128-byte status snapshot separates completed user operations from probe
results: fill selftests, triangle selftests, triangle probe failures, expected
and observed probe pixels, submitted dwords, completed fills/blits/triangles,
FIFO and idle timeouts, PCI identity and discovered resources. It records real
readback evidence rather than inferring use from a loaded filename.

Native copy capability (driver capability bit 2, value 4) has an independent
qualification. Before it is advertised, the backend fills two private linear
source rows with different CPU patterns, submits a native BCI copy through
explicit source/destination descriptors, verifies the copied pixels and
restores the private readback page after idle. A failed copy probe retains
fill capability but removes copy and triangle capabilities. Triangle operation
depends on working transfers and is not attempted with a failed copy probe.

## Bounded submission and CPU synchronization

Each BCI packet fits the 32-dword hardware FIFO and polls for sufficient space
before the first dword. MMIO is uncached and volatile, preserving x86 ordered
PIO writes. Alternate status polling is bounded at 65,536 reads per wait.
No engine reset is attempted on an uncertain timeout.

Native fill accepts one visible scanline up to the existing 16 KiB CVFF limit;
other valid layouts use the protected CPU fill service. Fill never overwrites
pitch padding or extends into another scanline. Successful return means the
GPU fill and its idle command completed. CPU row reads/writes, pointer save
and restore, presentation and page copies must synchronize with the native
engine first.

`cvsavage_page_copy(source,dest,rowbytes,rows,pitch)` accelerates the existing
page-copy service using the same PBD/SBD BCI transfer. It requires native copy
qualification, matching bound pitch and pixel format, aligned byte offsets,
equal source/destination X positions, visible row widths, disjoint full pitched
spans and extents inside all framebuffer pages owned by CVSESSION. It refuses
crossing a page's visible height, more than 64 rows or more than 16 KiB of
pixel data. Explicit descriptor bases allow copying between the first and
second front-buffer page. Unsupported geometry returns 1 without commands;
the monitor must synchronize before its CPU fallback. A hardware failure
returns 2 and retains ownership. Completed GPU copy increments the native
blit counter only after its idle command finishes.

`CVT3` is exactly 64 bytes: four 32-bit header fields (`CVT3`, size 64, zero
flags, zero reserved), followed by three X/Y/Z IEEE754 bit patterns and ARGB8888
colors. Packet contents are copied into the monitor's private storage before
validation. Coordinates must be finite, nonnegative and inside the active
viewport, Z must be in [0,1], and the containing rectangle is limited to 65,536
pixels. Validation uses integer bit decoding; ring 0 never changes x87 state.

For a triangle, the GPU copies the containing rectangle from the linear front
buffer into the tiled scratch surface, draws the triangle, then copies the
same rectangle back. The surrounding pixels remain intact, and the display
remains a BIOS linear surface. The full triangle operation synchronizes both
engines before returning success.

Bind returns 0 for unsupported or safely restored hardware, 1 after successful
native setup, and 2 when a failed operation retains hardware ownership. Fill
and triangle return 0 for completed native work, 1 for validation rejection or
an unavailable feature, and 2 for hardware failure. A retained failure forbids
CPU fallback and module unload until idle is proven. Release then restores all
saved registers and unmaps its private mappings; it never frees them while
the GPU could still be using them.

## Validation limits

The production source compiles with the same freestanding OpenWatcom flags as
CVSESSION. `test_session_gpu_savage.c` compiles the actual backend against a
bounded model of documented PCI/MMIO/BCI commands. It checks private probes,
RGB565/RGB8888 commands, tile/linear transfers, visible pixel preservation,
invalid commands and limits, no-S3 rejection, failed readback, bounded waits,
retained ownership and exact state restoration. This is a protocol test;
QEMU has no SuperSavage model and cannot certify the physical rasterizer.
Physical capability and operation counters must be collected on the T23.

## Read-only panel ceiling before framebuffer bind

`cvsavage_panel_probe()` provides internal LCD dimensions before selecting a
video mode. Its source and decisions are recorded in
[the panel research](t23-panel-detection-research.md). It permits only the exact
SuperSavage PCI identity, VGA display class, enabled PCI I/O decode and a valid
BAR0 resource. It caches the PCI identity, revalidates that identity, and reads
fresh register snapshots each time. It does not cache dimensions across mode
changes.

The probe reads CR6B and SR61/SR66/SR69/SR6E through standard indexed VGA ports;
it requires active LCD bit 1, two identical snapshots and plausible dimensions.
It uses X.Org's exact mobile decode equations. It preserves the CRTC and
sequencer index selectors and the PCI configuration selector. It performs no
register-data, MMIO, extended lock, clock or timing writes. Locked/unavailable,
all-FF, unstable, inactive and implausible reports produce unknown dimensions.

Status dwords at offsets 112/116/120 contain panel width, height and flags
(bit 0 means valid). DISPLAY_INFO exposes these at offsets 176/180/184. Offset
124 (DISPLAY_INFO 188) counts successful independent native copy readbacks.
The panel report is a selection ceiling for the active internal LCD, not a new
timing or a claim about an external monitor's maximum mode. BIOS VBE mode
validation and active access-path readback remain mandatory.

## System-shell diagnostic client

`SAV3D.COM` is a small native 3D demonstration and evidence collector. Run
`sav3d` from the F4 system shell while the shell retains its verified LFB
mapping. The shell must treat this installed diagnostic as a trusted system-VM
client instead of creating an independent DOS VM or ending its owned display
before EXEC. The client itself never changes the BIOS mode or binds/unbinds
the framebuffer.

The client discovers CVSESSION through INT 2Fh/1684h and validates QUERY's
magic, ABI version, active-session state, framebuffer ownership and native
triangle capability. It independently validates DISPLAY_INFO's backend 3,
ready state, exact S3 device identity and successful private fill/triangle
probe counts. Unsupported GPUs receive the explicit message
`Native S3 triangle unsupported on this active display.` No software fallback
is reported as a GPU result.

One hardware triangle uses the screen-space vertices (32,64), (192,64) and
(32,224), with red, green and blue vertex colors. The client requires a
completed triangle counter increment of exactly one, at least two completed
hardware transfers and actual submitted PIO words before reporting completion.
It waits for a key and returns to the shell for its ordinary repaint. A native
hardware error writes only the disk log and returns; it avoids DOS console
output while the owning shell must drain/release an uncertain engine.

`SYSTEM/VIDEO/GPU3D.LOG` is exactly 400 bytes:

| Offset | Contents |
| --- | --- |
| 0 | `CG3D` magic |
| 4 | 16-bit version `0x0100` |
| 6 | 16-bit header size 16 |
| 8 | 32-bit result: 0 completed, 1 unsupported, 2 session validation failed, 3 native operation/counter verification failed |
| 12 | 32-bit service error; AX is stored in its low 16 bits |
| 16 | 192-byte DISPLAY_INFO before submission |
| 208 | 192-byte DISPLAY_INFO after submission/error |

Unavailable snapshots remain zero. The client verifies complete log writes
and file close; it returns DOS errorlevel 0 on a completed logged demonstration,
1 when unsupported, 2 on session/hardware failure and 3 on a log failure.

`scripts/test_sav3d_client.py` assembles the real COM and executes its actual
instructions in Unicorn with mocked monitor/DOS services. Fourteen scenarios
verify preconditions, exact float/color packet bytes, completed counter proof,
400-byte evidence, rejection of stale/malformed metadata, missing native probe,
hardware failures, unavailable monitors and short/failed disk writes. Hardware
failure paths are checked to produce no framebuffer console output. This test
does not claim to emulate or qualify a physical SuperSavage.
