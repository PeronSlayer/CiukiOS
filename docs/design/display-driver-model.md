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
- **LINE:** solid single-pixel lines;
- **CURSOR:** monochrome 32×32 AND/XOR, which already exists, and colour ARGB up to 64×64 with a per-driver maximum size and alpha mode.

Every drawing command carries a destination clip list of up to 16 rectangles.

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

**[DM13] Scope boundaries.**
- **DOS VM monitor:** `src/vm` keeps its own native GPU session files. They are reviewed after the unified-DOS work (f3-63) merges.
- **Historical records:** dev diary entries, validation records, `savage-2d.md` and `supersavage-fullscreen.md` keep their names as records of that driver.
- **Power management:** the ThinkPad EC is a vendor quirk. Universal power management is covered by the peripheral census f1-102.

## Driver families

| Family | Status |
| --- | --- |
| Software engine (any VBE 2.0+ card with a linear frame buffer) | f2-101 |
| S3 Savage family | native, moves behind the registry in f2-101 |
| Others (ATI, NVIDIA, Matrox, Intel, 3dfx, S3 Trio/ViRGE, SiS, Trident, VIA, NeoMagic) | chosen by f2-102 |

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

## Amendment 1 (lead, 2026-10-22 23:28, Codex plan review)

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
