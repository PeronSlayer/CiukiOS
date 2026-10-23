# NeoMagic native 2D port

Implementation record: 2026-10-23, directive f2-121, on the f2-101 Part A
registry. No hardware or guest execution is claimed by this record.

The family census and command-semantics contracts were absent from this
branch; their binding sections and lead decisions were read in the lead's
read-only checkout. This record adds the result to the locally available
[display model family table](display-driver-model.md#driver-families).

## Primary-source comparison and decisions

| Source inspected | Repository comparison and resulting implementation |
| --- | --- |
| [Linux v6.12 `drivers/video/fbdev/neofb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/neofb.c), `neo2200_sync`, `neo2200_wait_fifo`, `neo2200_accel_init` (476–545) | The donor's FIFO-space calculation is explicitly broken and its idle loop is unbounded. Use the working full-idle alternative, a 20 ms limit plus 65,536 polls with a frozen clock, and the existing Part A pause callback after each 1 ms slice. Write depth into `bltStat` and byte pitch into both halves of `pitch`; no PLL, panel, clock, mode, interrupt, PCI command, DMA or master-BLT programming. |
| Same file, `neo2200_fillrect`, `neo2200_copyarea` (1361–1426) | Retain FIFO/skip-mapping control, source-copy ROP, foreground fill, byte offsets and extent-trigger order. COPY reverses both axes from bottom right when destination follows source. Ciuki checks all bounds and dimensions before submission. |
| Same file, `neo2200_imageblit` (1429–1489), especially 1443–1452 | The finite host aperture transport feeds one complete row of at most 256 pixels, with zero dword padding, before any wait/yield. Native-colour WRITE uses Part A's existing hook. Opaque MONO is a private primitive for the future Part C adapter. All 24 bpp mono widths below 16 refuse before MMIO, and transparent MONO refuses because the selected Linux path does not expose it. |
| Same file, dispatch (1492–1553), `neo_map_mmio` (1646–1690), `neo_init_hw` (1823–1924), PCI table (2128–2156); [Linux v6.12 `include/video/neomagic.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/neomagic.h) | Match table lists all nine contract IDs. Only NM2200/2230/2360/2380 have the donor F/C/Y path and accept probing. BAR0 owns the framebuffer; BAR1 is a 2 MiB, aligned MMIO aperture. Conservative usable VRAM is the minimum of the donor limit and firmware size. 32 bpp is refused: donor settings are guessed and acceleration initialization lacks that format. |
| [X.Org `src/neo_cursor.c`, commit `6661bdd4551e4e63e983685464a277845aed3012`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-neomagic/blob/6661bdd4551e4e63e983685464a277845aed3012/src/neo_cursor.c), `NeoHideCursor`, `neoRealizeCursor`, position/colour/memory helpers | Preserve 64×64 storage with separate per-row image/opaque planes and the address permutation. Convert legacy Ciuki 32×32 MSB-left opaque/image masks; crop negative positions sample by sample; hide wholly offscreen cursors. Expand RGB565 to RGB8, swap red/blue for cursor registers. SET hides and leaves hidden. Read back address, coordinates, colours and enable. The donor warns of lost MMIO maps: GR82 disable/readback is an independent safety proof. No panel-register writes are imported. |
| Owner's read-only Windows 98 Resource Kit, `build/rskt/wrkc24_3.htm`, “Display Driver Overview” | Confirmed that unsupported screen operations go to the DIB engine. Preserve Part A software fallback, per-operation counters, native-used policy and CPU exclusion. This is Windows 98 minidriver evidence, not an NT display architecture claim. |

Full downloaded donor files live only under `build/research/neomagic/`.
The finite [import manifest](../../src/kernel/drivers/video/neomagic/import.json)
records exact URLs, revisions and SHA-256 values. Every imported fragment
retains its origin and upstream notice. Credits and the installed notice
inventory include both donors.

## Enabled subset and identity

| Family | Match/acceptance | Result |
| --- | --- | --- |
| NeoMagic MagicGraph, `10c8` | `0001/0002/0003/0083/0004` nominate but refuse; `0005/0025/0006/0016` accept only the validated active owner | FILL, overlap-safe COPY, SYNC and native WRITE; optional 32×32 monochrome cursor when a separate safe VRAM page is available. RGB565 and packed BGR888 only. VBE still owns modes. |

The driver id is `neomagic`; its display name is `NeoMagic MagicGraph`.
The generic engine query supplies the existing Device Manager display-name
field and Control Panel's 2D-driver line. No UI lookup table or public ABI
change is needed. Legacy capability bit 0 denotes rectangles and bit 1 cursor;
WRITE is negotiated by its existing hook. Unsupported patterns use software.
No offscreen lease is advertised.

Opaque MONO and INVERT have driver-private primitives and register/pixel
fixtures. They are exercised during native-only activation comparison, but
cannot be exposed as user commands until f2-101d supplies the common Part C
hook signatures/capability allocations. This port invents no public numbers.
KEYED, SCALED, LINE, ARGB cursor and transparent MONO are not verified donor
paths in the selected implementation and stay unsupported.

## Ownership, activation and fault safety

The production registry proves a unique display function, the complete active
framebuffer lease/generation, BAR0 equality, 32-bit bounds, firmware VRAM and
absence of RAM/kernel aliases. The driver repeats that ownership gate before
activation and adds BAR1 type/alignment/extent/non-overlap checks, format and
silicon VRAM limits. Both firmware I/O and memory decode must already be on;
Ciuki never changes PCI configuration. Safe mode and exact `accel=0` refuse
before markers, claims, mapping or MMIO.

An exclusive GR-index/data claim supports the independent cursor disable.
The register aperture and, when available, one separate cursor page have
exclusive active claims and UC mappings. The cursor page is below 4 MiB,
outside the whole framebuffer lease, within both firmware and donor limits.
No destructive VRAM test or host memory DMA occurs. Unsafe stops retain all
leases and mappings. A proven clean stop revokes mappings and releases the
cursor, register and port leases before VBE claims the new framebuffer. The
private virtual windows are reused. No automatic reset or same-boot recovery
of quarantined submissions is permitted.

The existing Part A legacy attempt file is kept fail-closed. Any preexisting
nonempty or unreadable `accel.cfg` blocks activation, regardless of its key.
New records name this driver, but the Part B keyed storage migration is not
implemented here. Successful write, full-byte count, commit and close are
required before access; only successful activation clears it. Failure retains
the evidence, and all VFS work stays outside the presenter.

Setup submits nothing. All engine configuration and saved-block DM9 commands
run through `fbdev_accel_probe` under exclusive drawing ownership. The check
has a 100 ms command-check budget, followed by bounded idle cleanup, and compares native FILL, both COPY
directions, WRITE, INVERT and opaque MONO with
CPU reference pixels at each stage, then validates cursor register evidence
when advertised. It saves and restores only after idle and cursor-hide proof.
There is no software replay that could make this activation check pass.

Every engine error quarantines submissions and removes native capabilities.
After configuration, idle also requires the programmed depth and pitch to
read back, so an all-zero dead MMIO window cannot authorize CPU access.
`cpu_ready` is cached and nonwaiting. Quarantine by itself clears permission;
the fallback hook separately attempts bounded idle plus independent cursor
hide. Failure retains rejecting ops and prohibits CPU framebuffer/console
access. Successful idle permits software but never reactivates hardware in
the same boot. Failed COPY is not replayed by this driver. The generic
presenter retains its existing caller-reconstruction policy.

## Evidence and remaining qualification

`nice -n 19 python3 -B tests/host/test_neomagic.py` covers production registry
selection and lifecycle, donor register sequences, independent register-model
pixels against `swdib`, padded pitches, both overlap directions, transfer
padding, the small-mono refusal/software punt, cursor mask/colour/negative
coordinates and readback failures, dead/frozen-clock waits, cancellation,
unsafe guard retention, durable marker failures, startup DM9 corruption and
no-reprobe. Its second test compiles all three translation units with the
canonical i686 flags and checks external symbols with `nm`.

This is host evidence only. No pack model exists for this silicon. The lead
must review the BAR1/host-aperture transport and qualify LCD/Fn preservation,
GR82 disable, COPY overlap, colour/bit order and cursor positions on physical
NeoMagic hardware. Runtime activation is enabled behind DM9 as directed;
physical qualification is not replaced by the register model. Integration
builds, hardware evidence and Part C wiring remain lead/dependency work.
