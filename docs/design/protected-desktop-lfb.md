# Protected desktop framebuffer transport

The VBE 3.0 specification distinguishes the windowed and linear access models
with bit 14 of Set Mode and Get Current Mode. `PhysBasePtr` describes physical
memory; a protected operating environment must map that memory before using it.
The same specification explicitly excludes 2D and 3D acceleration primitives:
mapping an LFB is a faster CPU transport, not GPU acceleration.

Sources consulted on 2026-10-07:

- [VESA VBE 3.0, pages 6, 40 and 44](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf).
- [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html), protected mode and virtual-8086 privilege rules.
- [Linux Savage framebuffer definitions](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb.h), a separate chip-specific engine interface.

The existing CVSESSION monitor already maps a true linear framebuffer in ring 0
and provides bounded copy, row-copy and framebuffer-to-framebuffer operations.
Its restriction on banked framebuffer aliases is deliberate: a reported
`PhysBasePtr` does not prove that a banked mode enables that address. Preserve
the existing QEMU-only banked alias allowlist. A physical S3 adapter instead
needs an actual BIOS linear mode and successful active-mode readback.

Native-engine binding uses a backward-compatible 32-byte `CVFB` mode packet.
Its original 24-byte geometry and physical extent remain unchanged; offset 24
adds the VBE controller's usable VRAM byte count and offset 28 must be zero.
Old 16/24-byte callers remain supported for CPU transport and do not authorize
S3 offscreen allocations. The native backend checks the controller limit
against the chip's physical VRAM size and reserves scratch only below the
firmware's usable limit, rather than assuming all nominal 16 MiB is free.

The renderer provisionally records ownership before invoking BIND and invokes
UNBIND on a failed bind. Inactive, already-unbound UNBIND is an idempotent
success, so a failure before allocation does not create false quarantine.
An active DOS session, an undrained native command or failed mapping cleanup
returns an error and retains ownership. The renderer must complete release
before any BIOS mode change; it must suspend painting and retries while
ownership remains retained. QUERY's `FB_BOUND` reports a live protected
mapping, including this retained failure state.

GUI teardown waits for release before freeing compositor storage or changing
page bookkeeping. A failed release marks the UI inactive and enters a
foreground quarantine loop: enable interrupts, halt until the next interrupt,
then retry only UNBIND. It performs no BIOS call, rendering or external program
execution while ownership remains retained. Successful cleanup permits normal
mode initialization or DOS teardown to continue. Protected page-presentation
failure requests full mode recovery instead of silently switching the current
linear mode into a banked or VGA paint path.

Linux's upstream Savage definitions specify a 512 KiB MMIO register aperture
(`SAVAGE_NEWMMIO_REGSIZE = 0x0080000`). SuperSavage's command FIFO at `0x10000`
and status/control block above `0x48000` therefore cannot fit the previous
128 KiB internal mapping limit. The trusted CVSESSION MMIO mapper admits up
to 512 KiB while retaining physical-address, overflow and uncached-page
validation. This reserves virtual hardware pages; it does not allocate
512 KiB of conventional RAM or increase a DOS guest memory limit. Individual
native backends still validate the actual PCI device and BAR before asking
for a mapping.

The renderer must distinguish its real-mode flat-segment transport from a
protected CVSESSION transport. The latter must never enter the local CR0
switching routines. All reads, writes, fills, software-pointer saves/restores
and page copies must use the owned protected mapping. A missing or rejected
mapping cannot fall through to banked I/O while the firmware mode is linear;
the owner must recover the mapping or establish a banked firmware mode first.

## Bounded fill service

`VM_OP_FB_FILL` (`0x15`) extends the transport with an exact 32-byte `CVFF`
packet. Query advertises `VM_CAP_DESKTOP_FILL` (`0x00080000`) only in a monitor
which implements it. The packet has ABI version `0x0100`, size 32, and these
32-bit fields:

| Offset | Meaning |
| --- | --- |
| 0 | `CVFF` magic |
| 4 | 16-bit ABI version and 16-bit packet size |
| 8 | Framebuffer byte offset |
| 12 | Number of pixels |
| 16 | Native pixel value, low bytes used |
| 20 | Bytes per pixel, 1 through 4 |
| 24, 28 | Reserved; must be zero |

The system VM passes `ES:DI` and `CX >= 32`. The monitor validates the complete
packet span and copies it onto its private stack before writing VRAM. It rejects
zero count, multiplication or address overflow, a transfer larger than 16 KiB,
and an extent beyond the owned framebuffer. This keeps individual ring-0 work
bounded while allowing callers to split a large rectangle into scanlines.
Packet-validation errors leave VRAM untouched. Unsupported adapters use CPU
stores in ring 0, including exact three-byte pixels. The qualified SuperSavage
backend uses native GPU commands for supported scanlines after independent
hardware ownership, layout and completion validation. A hardware timeout can
follow submitted commands and retains ownership rather than permitting CPU
fallback.

`VM_OP_FB_PAGE_COPY` keeps its existing 32-byte `CVFC` ABI and its 16 KiB
payload / 64-row limits. The monitor first validates both disjoint pitched
bounding spans and snapshots the complete packet. The reviewed
[X.Org Savage copy commands](https://www.x.org/releases/individual/driver/xf86-video-savage-2.4.1.tar.xz)
and [Linux Savage FIFO completion handling](https://raw.githubusercontent.com/torvalds/linux/v6.2/drivers/gpu/drm/savage/savage_bci.c)
support a chip-specific copy between explicit linear descriptors. The monitor
passes binding-relative source, destination, row bytes, row count and pitch
to `cvsavage_page_copy`. Result 0 completes without CPU VRAM access; result 1
permits the existing CPU copy only after an idle check. Any other result
rejects CPU fallback. The backend's independent blit self-test and native
capability gate remain responsible for proving that BCI actually copied pixels.

## Native triangle service

`VM_OP_FB_TRIANGLE` (`0x16`) accepts a 64-byte `CVT3` packet: magic at 0,
32-bit size 64 at 4, zero flags at 8, zero reserved at 12, and three vertices
at 16, 32 and 48. Each vertex contains the stored IEEE754 binary32 bits of
screen-space x, y and z, followed by ARGB8888 colour. The protected service
validates and snapshots the entire guest packet before calling the native
driver. Coordinate, raster format and hardware-state validation belongs to
that driver. An unsupported or invalid submission returns an error; this API
does not silently substitute a software renderer.

`VM_CAP_NATIVE_FILL` (`0x00200000`) and `VM_CAP_NATIVE_TRIANGLES`
(`0x00100000`) are separate runtime engine capabilities. Their presence must
require an owned, ready native binding with the corresponding implementation.
They do not follow from the existence of CVSESSION, a BIOS LFB, or the CPU-fill
capability. All GPU submissions must finish before returning to code that may
paint the same framebuffer; CPU copy, pointer and fill paths explicitly check
engine idle before touching it. Failed completion retains ownership and
rejects CPU access and unbinding until cleanup can prove that it is safe.

`VM_OP_DISPLAY_INFO` retains its 192-byte packet. A physically owned
SuperSavage engine reports backend 3. Field 60 bit 31 identifies a read-only
128-byte `cvsavage_status_info` snapshot at offset 64; its low bits report
native engine capabilities. The snapshot includes ready state, probe stage,
PCI identity/BARs, memory bounds, completed fill/blit/triangle counts,
timeout counters and self-tests. A failed or unsupported probe still has
diagnostics but keeps backend 0 and lacks native capability bits. This
distinguishes an included binary, a matched device, engine ownership, and
operations completed on that engine.

## Transport validation

`scripts/test_framebuffer_fill.py` assembles the production fill/triangle and
guest-span/UNBIND routines with the pinned JWasm toolchain and executes their
actual 32-bit instructions under Unicorn. On 2026-10-07, 77 checks passed:
all four pixel sizes and boundary arithmetic, guest-page permission and packet
validation, native/CPU completion ordering, triangle snapshot immutability,
idempotent release versus retained engine/page-free failures, and exact pitched
page copies with native dispatch, immutable arguments and failure ordering. The complete
monitor also assembles without errors or warnings. GPU calls are mocked in
this transport fixture; these checks make no physical engine or performance
claim. Chip-specific protocol checks and the runtime private-VRAM self-tests
provide separate evidence for the native backend.

The GUI quarantine fixture (`scripts/test_gui_video_quarantine.py`) executes
the production release wait, mode switch, compositor lifetime and page-failure
routines. Eight CPU scenarios passed on 2026-10-07. They verify that failed
release yields with `STI/HLT`, restores the caller's interrupt flags only after
safe release, prevents BIOS mode changes and storage frees while ownership is
retained, and preserves the distinct checked banked fallback behavior.

The updated VBE CPU fixtures passed 18 metadata/transport groups and 38 AUTO
selection groups. They exercise the actual client against a scripted
CVSESSION far entry. They validate capability and system-VM eligibility,
true BIOS-linear mode selection with transport marker `vc_lfb=2`, exact
pointer/row/fill packets, lost-binding recovery, and provisional versus retained
binding failures. Protected cases reject all local CR0 switches and BIOS-bank
fallback. AUTO tests also check that safe cleanup permits the next ranked
candidate and retained ownership prevents all subsequent firmware mode sets.
These are instruction-level regression checks; they supply BIOS and protected
service responses and do not establish T23 display quality or acceleration.

The AUTO checks include the read-only native panel contract at DISPLAY_INFO
offsets 176/180/184. Checked SuperSavage panel bounds of 1024x768 or 1400x1050
only rank modes already supplied by firmware, including the greatest colour
depth at equal area. Malformed diagnostics, unsupported PCI identity, inactive
or unstable panel flags, unavailable service and all-FF responses retain the
XGA fallback. Valid EDID remains authoritative and bypasses the native panel
probe. The fixture makes no framebuffer binding or engine ownership during
this discovery stage.
