# Universal display driver model

Status: contract written by the lead on 2026-10-22 23:20 from the owner's
decisions of 2026-10-22 (about 23:10): CiukiOS must be universal, not specific
to the T23, and no generic code may name the S3 Savage. Agreed order:

1. GDI-style accelerated drawing commands with a software fallback (this
   contract, directive f2-101).
2. Native 2D drivers per graphics family, chosen by the census f2-102.
3. The NDIS miniport host (f6-04–f6-07).
4. The Setup CD and a hardware report for public trials.

Hardware 3D stays in F7, through Mesa (`f7-3d-acceleration-contract.md`).
A Codex plan review precedes the f2-101 code.

## Research

| Source | What it establishes |
| --- | --- |
| [Windows 9x video minidriver (OS/2 Museum)](https://www.os2museum.com/wp/windows-9x-video-minidriver-hd/) | Windows 95 added the DIB engine (`DIBENG.DLL`), which implements more or less all of GDI. The display minidriver sets modes and does housekeeping, and it "punts" any drawing it does not accelerate to the DIB engine. |
| [Microsoft: Copying bitmaps](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/copying-bitmaps) | The NT display model follows the same principle: a driver hands a complex raster operation back to GDI, which splits it into simpler ones. |
| [Haiku `Accelerant.h`](https://github.com/haiku/haiku/blob/master/headers/os/add-ons/graphics/Accelerant.h) | One hook table for every family. It has mode hooks (list, set, frame buffer, pixel clock, DPMS, EDID) and cursor hooks (`MOVE_CURSOR`, monochrome `SET_CURSOR_SHAPE`, colour `SET_CURSOR_BITMAP`, `SHOW_CURSOR`). It has engine hooks (acquire, release, wait idle, sync tokens) and 2D hooks (screen-to-screen blit, fill rectangle, invert, fill span, transparent blit, scaled filtered blit), each taking a list of rectangles. |
| [X.org EXA `exa.h`](https://gitlab.freedesktop.org/xorg/xserver/-/blob/master/exa/exa.h) | Drivers implement `PrepareSolid/Solid`, `PrepareCopy/Copy`, `Composite`, `UploadToScreen` and `DownloadFromScreen`. `Prepare*` may refuse, and EXA then draws in software. `MarkSync`/`WaitMarker` order CPU access after engine work. EXA manages off-screen pixmap memory. |
| [Linux `fb.h`](https://github.com/torvalds/linux/blob/master/include/linux/fb.h) | `fb_fillrect`, `fb_copyarea`, `fb_imageblit` (1 bpp expansion for console glyphs), `fb_cursor` and `fb_sync`. The generic `cfb_*` helpers are the software versions. |

All three families share solid fill, screen copy, 1 bpp expansion, image
upload, an engine fence and a hardware cursor. CiukiOS's `accel_ops`
(`src/kernel/include/ciuki/accel.h`) already has fill, copy, 8×8 pattern,
image write, a 32×32 monochrome cursor, sync and off-screen rows. The missing
parts are a driver registry, 1 bpp expansion, invert, colour-keyed and scaled
copies, a colour cursor, batching, and a software engine that implements every
command.

### Sites that name the Savage outside its driver (2026-10-22)

| Site | Today |
| --- | --- |
| `src/kernel/drivers/fbdev.c` | Calls `savage2d_init` and ten weak `savage2d_*` hooks (attempt marker, quarantine, snapshot, safe mode, fallback ops). |
| `src/kernel/include/ciuki/abi.h` | `CIUKI_ACCEL_DRIVER_SAVAGE` in `ciuki_accel_info`. |
| `src/kernel/core/devices.c` | Builds the "2D accelerator" record from `savage2d_summary` with driver `"savage2d"`. |
| `src/kernel/core/hardware_check.c` | `savage_check` with the Savage sweep. |
| `src/kernel/core/display.c` | The native full-screen path calls `supersavage_display_*` and logs `pci=5333:8c2e`. |
| `apps/desktop/desktop.c`, `compositor.c`, `protocol.c` | Notice text "YES — Savage", a comparison with `"savage2d"`, a driver-enum bound. |
| `apps/control/display.c`, `apps/control/app.c` | Hand-written PCI name tables, a "Savage engine" line. |
| `apps/gfxdemo/app.c`, `src/kernel/core/console.c`, `accel.h` | Driver name in output, comments, a prototype. |
| `README.md`, `docs/desktop.md`, `docs/build-and-run.md`, `docs/developer-api.md`, `Roadmap.md` | Describe the Savage as the accelerated path. |

## Decisions

**[DM1] Driver registry.** `<ciuki/display_driver.h>` defines
`struct display_driver`:
- a short ASCII `id` and a display name;
- a PCI match table (vendor, device, class mask);
- `probe`, which issues no engine command;
- `start`, returning `const struct accel_ops *`;
- optional mode provider (DM6), self-test (DM9), stop and snapshot hooks.

Drivers are listed in one table in `src/kernel/drivers/video/registry.c`.
fbdev binds the first driver whose match table and probe accept the PCI
function that owns the active VBE frame buffer. With no match, the software
engine (DM4) draws everything. Generic code never names a driver (DM12).

**[DM2] Command set.** Every accelerated driver must provide:
- **FILL:** solid rectangles, copy raster operation;
- **COPY:** screen-to-screen, safe for overlap in any direction;
- **SYNC.**

Each optional command has its own capability bit:
- **PATTERN:** 8×8 monochrome, which already exists;
- **WRITE:** host to screen, which already exists;
- **MONO:** a 1 bpp source expanded to foreground and background, with an optional transparent background, for text and masks;
- **INVERT:** XOR rectangles for focus and drag outlines;
- **KEYED:** copy with a source colour key;
- **SCALED:** stretch copy, nearest and optionally filtered;
- **STRETCH:** optional exact edge-sampled stretch, carried by SCALED with
  `CIUKI_FILTER_NEAREST_EDGE` and advertised by `CIUKI_CAP_STRETCH`;
- **LINE:** solid single-pixel lines;
- **CURSOR:** monochrome 32×32 AND/XOR, which already exists, and colour ARGB up to 64×64 with a per-driver maximum size and alpha mode.

Every drawing command carries a destination clip list of up to 16 rectangles.

### 101-d amendment: exact stretch and exclusive LFB export

Edge sampling is `origin + floor(destination_offset * source_extent /
destination_extent)` per axis, relative to the original unclipped rectangle.
Existing centre-sampled NEAREST and BILINEAR retain their semantics. Edge
stretch preserves packed pixel bits when formats match (as COPY does),
including the ignored X byte; differing formats use the software conversion.
Optional `accel_ops.stretch_rect` receives the immutable validated SCALED record and
retained targets. Its feature bit requires an actual native hook. Software
implements the same edge rule. Unsupported ratios/layouts refuse before any
submission; completion, clipping and partial-error recovery follow DM3/4.

The initial native implementation is restricted to 3dfx RGB565/XRGB8888
scanout row replication (`sw == dw`, `sh == 1`). The desktop uses it for runs
of equal composed rows, including Fit and 125% DOS windows, after the source
row completes. Every destination row is compared before admission. Application
scaling stays in software; COPY/CPU remain the fallback. This avoids assuming
an undocumented Bresenham phase. Register facts follow the
[Voodoo3 programming guide](https://bitsavers.trailing-edge.com/components/3dfx/Voodoo3_Programming_Guide_r1.4a_199906.pdf),
sections 7.2.8/16/21/23.2, 8.2.6 and 8.4.4, corroborated by
[X.Org tdfx](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/blob/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src/tdfx_video.c).
Other families retain refusal; see `../validation/101-dos-video-native.md`.

An optional mode-provider `lfb` hook exports a kernel-only physical extent
after packed mode entry: physical base, kernel alias, extent, geometry/pitch/
bpp and the alias's PTE cache bits. A page-aligned extent must fit the
provider's exclusive framebuffer lease. Geometry and indexed-8/RGB565 layout
must exactly match the synthetic VBE mode. MTRR-backed WC uses PCD=PWT=0;
otherwise UC uses both, matching every kernel alias. No PAT state is changed.

The provider stays pinned until guest aliases are revoked. Desktop grants
are suspended, acceleration stopped and composition excluded. The VM parks
before initial copy/PTE replacement. Original pool frames remain pinned and
charged, with unchanged commitment/mapping counts; borrowed MMIO is not RAM.
Revocation holds the VM, removes guest permissions from all borrowed PTEs
atomically, drains stores, copies the page-rounded visible extent to retained
pool frames, then restores the saved PTE frames/flags atomically (setting D
for the first windowed sample) before restoring the
desktop. Stop/fault/mode-change follow the same protocol; a park timeout keeps
the VM suspended. Kernel transfers/screenshots use the exported alias while
direct. Any export/layout/cache/PTE refusal leaves the copy path. Cache alias
and TLB rules follow [Intel SDM Vol. 3A](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
sections 4.10 and 12 (formerly 11), using the existing MTRR policy.
Mode-change continuations pin the global display transaction across copy-back
service points; a second return cannot restore the provider underneath them.
The mode-change pin is independent of the display worker's busy ownership:
neither continuation saves/restores the other's state. A leave already waiting
at a checkpoint or in park must recheck this pin and revocation completion
after yielding. An in-progress copy-back is a deferred return, including after
park timeout; the VM and provider remain pinned until `pages == 0 && !copying`
and the mode-change continuation releases its pin. Poll continuations also
recheck the pin after checkpoint waits before requesting a leave. These are
software lifetime requirements in addition to the unchanged Intel SDM
4.10.4.2/4.10.4.4 invalidation rules; TLB invalidation alone cannot protect a
kernel alias still being read by another continuation.
Kernel pool transfers recheck the direct lease after a resolving service point.

**[DM3] Batches and fences.** A new user request, `CIUKI_ACCEL_BATCH`, takes
up to 64 fixed-size command records and runs them under one presenter
acquisition. It returns the number completed and a fence sequence. The CPU
touches video memory that the engine wrote only after waiting on the fence,
as in EXA `WaitMarker` and Haiku sync tokens. The existing single requests
remain as wrappers. The presenter's 1 ms completion slicing
(`win98-class-resource-management.md`) does not change.

**[DM4] Software engine as the reference.** `src/kernel/drivers/video/swdib.c`
implements every DM2 command with the same pixel semantics. That covers
clipping, overlap, colour key, MSB-left 1 bpp order, scaling rounding, 16, 24
and 32 bpp, and system-memory targets. It is the CiukiOS equivalent of the
Windows 9x DIB engine.

A driver may refuse one call's parameters with `-EOPNOTSUPP`. The registry
then syncs the engine for the overlapping destination, and the software engine
draws that call. Counters record engine and punted commands per operation.

The software engine draws everything in these cases:
- no driver matches, which covers any VBE 2.0+ card with a linear frame buffer;
- the driver does not advertise the command, or refuses its parameters;
- the engine was quarantined, with no reprobe in the same boot;
- an off-screen target is missing;
- safe mode or `accel=0`.

**[DM5] Video memory.** The off-screen manager is generic. The driver reports
the usable aperture beyond the visible frame and its alignment. The registry
leases row ranges as in f2-92 `CIUKI_ACCEL_OFFSCREEN` and revokes them on a
mode change, using the generation.

**[DM6] Mode providers.** VBE through the BIOS VM is the universal mode
setter. A driver may add a native mode provider for DOS full screen (f3-32).
`display.c` calls the bound driver's provider through a generic interface, and
the SuperSavage provider becomes the S3 Savage driver's provider. Without a
provider, the firmware VGA path is used, as today.

**[DM7] ABI identity.** `ciuki_accel_info` replaces the driver enum with:
- an engine kind: none or software, or native;
- `char driver_id[16]`;
- the PCI vendor and device;
- the punted-command counter.

Static asserts, the SDK and `docs/developer-api.md` are regenerated, and
`CIUKI_ACCEL_DRIVER_SAVAGE` disappears. The "2D accelerator" device record
carries the driver id.

**[DM8] Durable attempt marker.** The f1-27 marker becomes generic and is
keyed by driver id and PCI id. A crash during one driver's engine bring-up
disables that driver on the next boot, with a reason, and the software engine
takes over.

DM8 amendment (lead decision, 2026-10-23): retry and the first-boot check
work for every registered driver. Each `struct display_driver` declares in
`attempt_files` every durable attempt file its sources read besides the
registry keyed marker (the shared `accel.cfg` only for drivers that honour
it; one `%04x` expands to the PCI device id). Before every activation the
registry (`display_attempt_prepare`, worker context) first consumes a user
retry request, `C:/system/settings/accel-retry.cfg` with exactly
`driver=<id>\npci=<vvvv>:<dddd>\n`: for the bound key it truncates the keyed
marker and every nonempty declared file, then the request; another key or
a malformed request is consumed without clearing. A nonempty attempt file
then blocks the attempt before the driver's own hook with the registry
reason `attempt_retained`, which outranks the driver's reason in the engine
record. Control Panel "Retry acceleration after restart" writes that
request from the engine record (no driver path in applications); first
boot reports software mode from the engine record's readiness and
`attempt_retained`, never from a marker file. Host evidence:
`display_attempt_test.c` (Savage and Trident declarations, storage cuts),
`display_driver_test.c` (integrated status), and
`test_display_driver_model.py` `test_attempt_files_cover_every_driver`.

**[DM9] Hardware check.** One generic test covers every bound driver. Each
advertised command runs on a saved screen block, and the read-back is compared
with the software engine. The test covers fill, copy in both overlap
directions, mono, write, keyed and invert, then the cursor if advertised.
Driver-specific sweeps, such as the Savage configuration sweep, become the
driver's optional self-test hook and are reported under the driver's display
name.

**[DM10] User interface.** The desktop notice reads "2D acceleration: YES —
<display name>" or "NO — <reason>", from the device record. Control Panel →
Display shows:
- the adapter name from `/System/hwids/pci-ids.json`, with no hand tables;
- the driver's display name;
- the state: active, quarantined with reason, or software;
- the engine and punted counters.

`gfxdemo` prints the driver id.

**[DM11] Driver layout and sources.** Each native driver lives in
`src/kernel/drivers/video/<id>/` with an origin and licence header. Upstream
families come from the census f2-102: Haiku accelerants (MIT), Linux fbdev
(GPL-2.0) and X.org drivers (MIT). All three are compatible with the
GPL-2.0-only kernel, and copied fragments carry their origin. The Savage
driver moves to `src/kernel/drivers/video/s3savage/` after f2-99 merges, as
part B of f2-101.

**[DM12] Enforcement.** A host test fails when a driver or chip name appears
in generic code. The names checked include savage, supersavage, mach64, rage,
radeon, nvidia, riva, matrox, mga, i810, i830, voodoo, tdfx, trident, sis,
neomagic and via. The generic code covers:
- `src/kernel/core`;
- fbdev;
- the registry;
- `src/kernel/include/ciuki` except driver headers under `video/`;
- `apps/`;
- `sdk/`.

Documentation describes "native 2D drivers" and lists the families in one
table in this file.

Scope amendment (lead decision, 2026-10-23): the check covers generic
*display* code only. Audio (`ac97.h`, `audio_backend.h`), ACPI/APM power
(the Control Panel Power page) and non-display device records (audio owners,
ATA chipset families) legitimately name chip vendors. The test exempts the
two audio headers as whole files and the non-display lines of mixed files
(`devices.c`, `apps/control/app.c`) as exact lines; it also fails when an
exemption no longer matches the source. Display generic code stays free of
driver names.

**[DM13] Scope boundaries.**
- **DOS VM monitor:** `src/vm` keeps its own native GPU session files. They are reviewed after the unified-DOS work (f3-63) merges.
- **Historical records:** dev diary entries, validation records, `savage-2d.md` and `supersavage-fullscreen.md` keep their names as records of that driver.
- **Power management:** the ThinkPad EC is a vendor quirk. Universal power management is covered by the peripheral census f1-102.

## Driver families

| Family | Status |
| --- | --- |
| Software engine (any VBE 2.0+ card with a linear frame buffer) | f2-101 |
| S3 Savage family | relocated in f2-101b; f2-112 adds family transports with host validation, physical qualification pending |
| ATI Radeon R100/RV100 (discrete R100/RV100/RV200 primary functions) | f2-103: MMIO FILL/COPY/SYNC and monochrome cursor; VBE modes; host fixtures, runtime pending; [source decisions](../validation/f2-103-radeon-r100.md) |
| ATI Radeon R200/RV250/RV280, R300/R350/RV350/RV380, R420/R423 (X800) | f2-106: explicit discrete primary IDs; separate subgroup cache/feature gates; MMIO FILL/COPY/SYNC, PATTERN/WRITE and monochrome cursor; saved-pixel native comparisons, VBE modes; host fixtures pass, physical qualification pending; [source decisions](../validation/f2-106-radeon-r200-r400.md) |
| NVIDIA Riva 128 (NV3, `12d2:0018`) | f2-120: independent PIO FILL/COPY/SYNC/PATTERN and monochrome cursor; lead-approved transport behind DM9, VBE modes; host evidence, physical qualification pending; [transport and limits](riva128-2d.md) |
| NVIDIA TNT/TNT2, GeForce256/2/3/4, FX/GeForce6 | `nv2d`, NVIDIA TNT-GeForce6: f2-104 PIO approved behind DM9; f2-105 VRAM ring approved by amendment 1, activation behind DM9; VBE modes |
| Matrox G200/G400/G450/G550 | f2-110: Linux v6.12 native FILL/COPY/SYNC, DAC cursor and private mono preparation; host evidence, physical qualification pending; [port record](matrox-2d.md) |
| VMware SVGA II (`15ad:0405`) | f2-111: native FIFO FILL/COPY/SYNC and legacy cursor, host verified; runtime qualification pending. VMSVGA without native FILL and earlier `0710` use software; see [implementation record](vmware-svga.md). |
| Trident Blade / CyberBlade (f2-113) | Native Blade/XP FILL/COPY/SYNC, activation read-back guard; host evidence, hardware pending. Shared 1023:8400/8500 owned here. See [port contract](trident-blade-2d.md). |
| ATI Rage 128 / Rage 128 Pro (including donor-listed Mobility) | f2-115: native FILL/COPY/SYNC and optional monochrome cursor behind part A; host verified, lead runtime/physical qualification pending; [evidence](../validation/f2-115-rage128.md) |
| Cirrus Logic GD5430/GD5434/GD5436/GD5446 | f2-116 native FILL/COPY/SYNC; host qualification, runtime pending; VBE modes |
| Intel i810/i815 (`8086:7121/7123/7125/1132`) | f2-119 native FILL/COPY/SYNC and optional legacy cursor; kernel-owned DMA-pinned ring/cursor pages in proved-free GTT entries; saved PTEs restored on stop; host evidence, physical DM9 pending; [transport](intel-legacy-2d.md) |
| Intel i740 (`8086:7800/00d1`) | f2-119 separate native LP_FIFO FILL/COPY/SYNC and optional legacy cursor; MONO disabled; host evidence, physical DM9 pending; [transport](intel-legacy-2d.md) |
| Chips & Technologies HiQV 65550/54/55 | f2-122: native F/C/SYNC, optional pattern/staged write/mono cursor at 16/24 bpp; DM9 activation, host evidence, physical qualification pending; [port record](chips-hiqv.md) |
| Intel i830/i845G/i855GM/i865G/i915G | f2-107: registered FILL/COPY/SYNC, owned GTT ring/HWS/scratch, breadcrumb and bounded stop; host models pass. i845G/i855GM/i915G candidates with qualified firmware ownership/cache publication; i830M/i865G remain refused for unproved chipset flush. Cursor/INVERT gated; silicon/DM9 validation pending. See [reservation contract](intel-ring-gtt-reservation.md). |
| VIA UniChrome / UniChrome Pro | f2-108: native FILL/COPY/SYNC, staged WRITE when UMA scratch is proved; VBE modes, software cursor; shared `1023:8400/8500` belong exclusively to Trident f2-113. Host evidence only; see [port record](via-unichrome.md). |
| SiS 300/540/630; separate SiS 315/330 and 550/650/660 derivatives | f2-109 native MMIO FILL/COPY/SYNC, PATTERN and legacy monochrome cursor; private INVERT; host model and hardware gates in [validation](../validation/f2-109-sis.md) |
| S3 Trio64 / Trio64V+ / UV+ / V2 (`s3trio64`) | f2-118: separate Haiku r1beta5 RGB565 backend; host evidence in `s3-trio64-virge.md`; hardware qualification pending |
| S3 ViRGE / VX / DX/GX / GX2 / MX / MX+ (`s3virge`) | f2-118: separate Haiku r1beta5 RGB565 backend, optional mono cursor; host evidence in `s3-trio64-virge.md`; hardware qualification pending |
| NeoMagic MagicGraph | [f2-121](neomagic-2d.md): NM2200/2230/2360/2380, Linux v6.12 F/C/Y/W, X.Org monochrome cursor, native-only activation check; older chips and 32 bpp use software; physical qualification pending |
| ATI Mach64 / Rage Mobility | f2-114: native FILL/COPY/SYNC and optional legacy cursor; VBE modes; [transport](mach64-2d.md); physical qualification pending. |
| 3dfx Voodoo3 / single-chip VSA-100 | f2-117: native FILL/COPY/SYNC and optional legacy cursor; VBE modes; [transport](tdfx-2d.md); physical qualification pending. |

## Acceptance for f2-101

Host evidence:
- registry selection, ordering, probe failure and fallback;
- golden software-engine pixels for every command at 16, 24 and 32 bpp, including clip lists, overlap, bit order, colour key and scaling;
- punt semantics with a recording fake driver;
- batch ABI: partial completion, malformed records and fences;
- ABI asserts;
- the existing Savage host tests unchanged and green through the registry;
- user-interface text tests;
- the DM12 name test.

Runtime evidence, run by the lead: QEMU standard VGA binds no driver and uses
the software engine, and on the T23 the Savage binds through the registry.

## Amendment 1 (lead, 2026-10-22 23:26, Codex plan review)

A Codex `gpt-6.1-sol` plan review returned 19 findings with code
evidence; the lead accepts all of them. The binding changes:

1. **Quarantine and CPU safety (review 1).** Quarantine disables submissions;
   it does not prove that the CPU may touch video memory. The rejecting guard
   stays until idle or stop is proved. A failed proof forbids CPU drawing and
   console/panic frame buffer access (serial diagnostics remain). `cpu_ready`
   is mandatory, cached and nonwaiting. The software engine draws only after
   that proof.
2. **Lifecycle and locks (review 2, 13, 15).** The registry header specifies
   the signature and lock context of every hook: eligibility, activation,
   fallback guard, marker begin/end/failure, quarantine, record, summary,
   snapshot and one authoritative stop.
   - Deferred initialization stays outside the presenter lock, and activation pixel probes are serialized through fbdev.
   - VFS/ATA I/O and deferred records happen outside the presenter lock.
   - Generic policy checks safe mode and the exact `accel=0` option before any activation, marker change, resource claim or self-test, and records a software/disabled summary.
   - Match tables only nominate candidates. Ownership of the frame buffer is proved from validated PCI resources and the frame buffer extent, and ambiguity selects software. Existing eligibility gates are preserved.
   - Clean probe refusal, safe start failure and unsafe start failure are distinct. No other driver is tried after unproved hardware activity.
3. **Part A preserves the driver (review 3, 4).** The adapter forwards the ops
   table the driver returns and references no static driver function.
   - Existing `accel_ops` field order and signatures stay; new optional hooks are only appended, and clipping is layered above the legacy hooks.
   - Marker orchestration is abstracted without changing its storage or authorization.
   - Keyed storage (DM8) and legacy-marker migration move to part B with a reviewed export change. An interrupted legacy attempt is never cleared because it lacks a key.
4. **ABI compatibility (review 5, 6, 19).**
   - The legacy request, operation numbers, capability bits (bit 0 hardware rectangles, bit 1 hardware cursor), WRITE return values (0 engine, 1 CPU) and the 40-byte `ciuki_accel_info` stay as they are.
   - DM7 becomes a new size- and version-negotiated generic query; the adapter translates the legacy snapshot.
   - The device record gains bounded fields for driver display name, reason, readiness and native-used, including software-only boots. "YES" still requires an actual native draw.
   - Software-supported operations are reported separately from native capabilities.
   - Extended desktop messages (`DESK_GFX_INFO`) are versioned, with encoder, decoder, Control Panel, gfxdemo, SDK headers and libciuki wrappers updated together.
5. **Cursor (review 7).** The desktop keeps owning the software cursor. Without
   a native cursor, cursor requests return `-EOPNOTSUPP` and the hardware
   cursor bit is never advertised. SET hides before replacing; stop and
   quarantine hide or disable the native cursor before the software takes over.
6. **Punting (review 8).** `-EOPNOTSUPP` means no submission and no pixel
   change; only that permits automatic replay in software. A fault after
   submission needs completion proof and operation-specific recovery.
   Overlapping COPY and other non-idempotent operations are never replayed
   blindly (today's `-EAGAIN` and caller reconstruction stay).
7. **Synchronous part A (review 9, 10, 11, 16).** Part A stays synchronous:
   success proves completion.
   - Before CPU access, conflicting engine work on every source and destination is synchronized, and CPU frame buffer writes drain before engine access and before ownership is released.
   - Batches, fences, clip lists, MONO/INVERT/KEYED/SCALED/LINE, colour cursors and system-memory targets move to **part C**. Part C needs a written semantic specification first: pixels, aliasing, clip union, formats, line endpoints, filtering, alpha, pattern origin and errors. It also defines the batch layout, limits, grant checks, staging outside display locks, prefix completion and lock lifetime, so that drawing claims and in-flight state hold across every presenter release.
8. **Off-screen memory (review 12).** Software touches only validated
   CPU-mapped targets. A missing or revoked engine-only target fails, and the
   caller reconstructs it. Part A keeps today's verified off-screen spans and
   driver-owned claims, revoked on quarantine as well as on a mode change.
9. **Mode providers (review 14).** Adapter and provider binding are
   independent of active 2D ops. The provider is pinned for the whole
   full-screen transaction with its supported, prepare, begin, write, end and
   complete hooks. A preflight failure on a natively managed adapter stays
   fail-closed; firmware fallback happens only through the existing mediated
   recovery. `accel=0` does not change full-screen recovery.
10. **Hardware check (review 17).** The comparison proves native execution,
    with fallback disabled or recorded separately. It holds exclusive drawing
    ownership, saves and restores only after proven idle, and is bounded by
    deadlines and cancellation. It preserves retained diagnostic markers and
    same-boot quarantine. Cursor state is checked through driver evidence,
    separately from frame buffer read-back.
11. **Build and tests (review 18).** Video sources are listed explicitly in
    `scripts/build_kernel.py` with collision-free object names. Mechanical
    fixture and linkage changes are allowed if behavioural assertions stay.
    Integrated fake-driver tests cover:
    - marker failures and unsafe quarantine;
    - presenter re-entry and console exclusion;
    - sliced waits and mode races;
    - revoked off-screen rows;
    - legacy binary layouts and SDK/protocol round trips.
12. **Name enforcement (review 19).** The DM12 test checks that generic code
    has no driver-specific policy or calls. It uses explicit exceptions for
    declarative registration, private compatibility headers and vendored
    sources, not unrestricted substring matching. Control Panel reuses its
    existing `hwid_name` lookup.

## Part A implementation interfaces

Primary-source comparison for this implementation:

- The owner's Windows 98 Resource Kit, chapter 24, `wrkc24_3.htm`, “Display
  Driver Overview” (read from the lead's read-only `build/rskt`), describes
  DIB-engine drawing for operations a minidriver does not accelerate. This is
  the Windows 98 reference; the NT DDI is supporting evidence for the dispatch
  pattern, not a claim that the two driver architectures are interchangeable.
- [Microsoft EngCopyBits](https://learn.microsoft.com/windows/win32/api/winddi/nf-winddi-engcopybits)
  specifies bounded, lower/right-exclusive destination rectangles and packed
  standard formats. The named “Copying bitmaps” page could not be retrieved;
  this official DDI page was checked instead.
- [Haiku Accelerant.h](https://raw.githubusercontent.com/haiku/haiku/master/headers/os/add-ons/graphics/Accelerant.h)
  separates mode, engine, synchronization and optional drawing hooks. Part A
  keeps the immutable mode provider independent of 2D policy and pins its
  pointer through begin, write, end, mediated recovery and complete.
- [X.org EXA](https://raw.githubusercontent.com/XQuartz/xorg-server/master/exa/exa.h)
  permits refusal before drawing, orders overlapping copies by direction, and
  coordinates CPU access to sources and destinations. Part A synchronizes the
  complete synchronous engine before CPU access. `-EOPNOTSUPP` is a clean
  refusal; failed submitted COPY retains caller reconstruction with `-EAGAIN`.
  Existing idempotent FILL/PATTERN/WRITE recovery still requires proven idle.
- [Linux fb.h](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/fb.h)
  separates fill/copy/image/cursor and idle hooks. The existing integer CPU
  renderers move to `swdib.c`; no upstream rendering code is copied.

The new `CIUKI_ACCEL_QUERY_ENGINE` uses the unchanged 308-byte request:
`colour` selects version 1, `background` offers output bytes, and EDX points
to the output. Version 1 requires at least 232 bytes and copies exactly 232;
unsupported versions and short buffers fail before output. Legacy QUERY,
40-byte snapshots, operation numbers 0–8, capability bits and WRITE returns
are unchanged. The legacy native enum has a generic public name; the frozen
driver's old spelling is private source compatibility.

`DESK_GFX_QUERY_ENGINE`/`DESK_GFX_INFO_ENGINE` opt in to the generic version-1
payload; the latter is a 24-byte header plus the exact 232-byte record. Legacy
64-byte `DESK_GFX_INFO` remains accepted. Device inventory is version 2 with
504-byte records (new display name/readiness/usage/CPU-safety fields); its
existing size/version gate rejects old inventory buffers before copying.
The stable accelerator record id is `CIUKI_DEVICE_ACCELERATOR_ID`. Software
and native per-operation counts are separate; only completed presenter draws
establish `native_used`. Cursor/setup/diagnostic commands do not establish it.

The read-only ownership gate requires exactly one display PCI function,
32-bit enabled memory decode, a matching driver-declared framebuffer BAR,
the full validated VBE extent within firmware VRAM, and no RAM/kernel alias or
32-bit wrap. The exact active `fbdev` MMIO lease, bounds and generation must
also agree with that PCI BAR. A mismatched or ambiguous function is never activated. Driver
eligibility remains an additional gate. BAR sizing is not performed through
configuration writes. Provider selection does not call eligibility, marker or
activation hooks, so `accel=0` does not change full-screen recovery.

The host suite is `nice -n 19 python3 -B tests/host/test_display_driver_model.py`
(UBSan); `CIUKI_DISPLAY_ASAN=1` additionally enables ASan with default leak
checking. `tests/host/test_display_driver_names.py` enforces DM12 using C token
boundaries and explicit declarative/driver-tree/vendored/audio exceptions.
Neither suite builds an image or executes a guest. Part B supplies keyed
markers and file moves below; part C retains all newly proposed command/batch
semantics.

Presenter unlock refreshes the driver's cached legacy snapshot while the lock
is held (never during deferred initialization), then releases the lock.
Inventory reads use that cache. Observed quarantine latches the generic
same-boot activation block and revokes usable off-screen spans through native
capabilities. Native submission stays excluded until marker clear finishes.
The notice waits for a pending decision and still requires a completed native
presenter draw before reporting YES.

The H-page cursor check uses a fresh diagnostic activation (no prior desktop
cursor owner). The adapter loads a transparent shape, moves it, enables it and
hides it again, under exclusive drawing ownership and deadline/cancellation
checks. Existing driver callbacks verify the cursor base and enable register;
MOVE supplies submission evidence, not coordinate-register readback. The
cursor-evidence bit is separate from the four framebuffer command bits.

## Part B implementation: private driver tree and durable keys

The native sources, private headers and registration adapter now live in
`src/kernel/drivers/video/s3savage/`. The canonical builder explicitly lists
the four native translation units with `s3savage_*` object names, alongside
`video_registry`, `video_attempt` and `video_swdib`. The move changes includes
and build/test paths, without changing register programming, drawing, mode
transitions or resource qualification. The name test also scans the video
tree; former private-header exemptions outside the driver directory are gone.
Only the exact declarative registration lines in the generic registry remain
an exception for this family.

`display_attempt_marker` is the registry's worker-only persistence interface.
The driver exports `savage2d_attempt_begin/end/failed` and
`savage2d_check_begin` remain the adapter boundary. BEGIN still runs the same
eligibility and production EngineConfig checks; successful write, commit and
close still grant only one activation. The adapter pins the PCI vendor/device
before yielding to storage and uses that key for END and failure annotations.
END and annotation reject retained attempts and failed BEGINs. No VFS I/O
moves under the presenter, no lock context changes, and no new panic path is
introduced.

Storage is `C:/system/settings/accel-<id>-<vendor>-<device>.cfg` with four
lowercase hex digits for each PCI component. Driver ids are bounded to 1–15
lowercase ASCII letters, digits, `_` and `-`; filenames encode the full key
without hashing, truncation or case aliases. Any nonempty keyed record blocks
activation, even on a read-only volume. A missing, unreadable or uncommittable
settings store cannot authorize hardware. A busy flag protects the single
static VFS table against re-entry while filesystem calls yield. Successful
completion truncates only the pinned key; failure annotation never truncates.

The legacy `accel.cfg` path is private adapter data associated with this
driver's current, uniquely identified supported PCI function. Other drivers
do not receive that mapping. A nonempty legacy record is always reported as
a retained attempt. When writable and at most 240 bytes, migration reads its
exact bytes, adds `migrated=legacy\n`, writes the keyed replacement and requires
both commit and close before truncating/committing/closing the old file. A
`[accel] marker migration` log includes driver id, PCI id and the legacy path.
The migrated record still blocks this boot and subsequent boots. Any failure
leaves at least the old or the new guard; read-only, oversized and unreadable
legacy records remain untouched. If a keyed guard already exists, both records
are retained. A retained diagnostic attempt cannot clear either guard.

Primary-source comparison for this change:

- The read-only Windows 98 Resource Kit, chapter 24, `wrkc24_3.htm`,
  “Display Driver Overview”, describes fallback when a display minidriver
  fails to initialize. Keep that fallback behind durable attempt authorization;
  the keyed on-disk protocol is CiukiOS policy, not a claimed Windows format.
- [S3 RE046-B, sections 1 and 4](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf)
  and [X.Org `src/savage_driver.c`, `SavageMapMem` (NetBSD xsrc ref `trunk`)](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
  retain the existing family-specific BAR and transport decisions. This change
  neither programs new registers nor changes those sequences.
- [Haiku `headers/os/add-ons/graphics/Accelerant.h`, ref `master`](https://raw.githubusercontent.com/haiku/haiku/master/headers/os/add-ons/graphics/Accelerant.h),
  [X.org `exa/exa.h`, ref `master`](https://raw.githubusercontent.com/XQuartz/xorg-server/master/exa/exa.h)
  and [Linux `include/linux/fb.h`, tag `v6.1`](https://raw.githubusercontent.com/torvalds/linux/v6.1/include/linux/fb.h)
  separate driver lifecycle and drawing/synchronization hooks. Persistence
  remains outside drawing and mode transactions. No upstream code is copied
  or vendored; the independently implemented driver origins and GPL notices
  are preserved.

The host display-model suite includes committed-versus-cache storage tests
for driver/vendor/device isolation, interruption, status annotation, migration,
every migration I/O cut, read-only media, oversized/unknown legacy contents,
short reads/writes, invalid ids and yielding re-entry. The production register
suite also checks that retained diagnostic END/annotation cannot clear a
marker. These are host models, not evidence of hardware or guest execution.

## f2-112 family transport interfaces

The registry appends the optional, read-only `framebuffer_bar_for(pci)` hook.
Its result selects the BAR to check with the existing extent, active-lease,
generation, RAM exclusion and ambiguity rules. It cannot establish ownership
by itself. Legacy adapters retain `framebuffer_bar` and need no changes.

DM9 runs its native-only software-reference comparison when an optional driver
configuration sweep cleanly returns `-EOPNOTSUPP`, with no trials and restored
state. A retained attempt still skips activation; unsafe or partially attempted
sweeps still fail. The established configuration sweeps and their two-depth
qualification remain unchanged. A missing optional sweep follows the same clean
path. The keyed guard must be cleared successfully before the comparison's
fresh durable activation. See [f2-112 host validation](../validation/f2-112-savage-family.md).
