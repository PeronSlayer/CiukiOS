# Riva 128 (NV3) native 2D

Implementation decision, 2026-10-23. Scope: f2-120; VBE retains scanout.

## Primary-source comparison

The donor is X.Org xf86-video-nv commit
[`0b6619fec39fada995646fb95bb25e72d3c9a491`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/tree/0b6619fec39fada995646fb95bb25e72d3c9a491/src).
`src/kernel/drivers/video/riva128/import.json` pins the downloaded files.

- [`riva_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_driver.c)
  accepts only SGS NVIDIA Riva128, `12d2:0018`, and excludes the final 32 KiB
  of VRAM. Linux [v6.12 `riva/fbdev.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/riva/fbdev.c)
  agrees on that NV3 ID; its TNT entries belong to other architectures.
- [`riva_setup.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_setup.c)
  maps the PIO aperture at BAR0+8 MiB, but NV3 instance memory at **BAR1+12 MiB**.
  The latter must never be treated as an NV4 BAR0 PRAMIN mapping.
- [`riva_hw.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_hw.h),
  [`riva_tbl.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_tbl.h),
  [`riva_hw.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_hw.c)
  define the NV3 object hashes, contexts, DWORD-indexed tables, subchannels,
  local framebuffer translation, PGRAPH busy bit at +0x6b0, and cursor at
  instance offset 0x7800. Port those fragments, with bounded waits, rather
  than importing the donor's unbounded FIFO macros or full mode setter.
- [`riva_xaa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_xaa.c)
  packs rectangle XY/WH in the opposite halfword order from COPY. COPY
  uses upper-left coordinates for both overlap directions; silicon selects
  traversal. PATTERN has absolute screen origin, little-endian pattern
  bits, and a special RGB565 colour expansion. FILL uses native colour.
- [`riva_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_cursor.c)
  expands source/mask to 32x32 A1RGB555, packs two pixels per DWORD, and
  writes signed 16-bit cursor coordinates at PRAMDAC+0x300. Adapt Ciuki's
  MSB-left opaque mask/image convention explicitly. No invert/ARGB cursor
  capability is implied by this binary-alpha hardware.
- [envytools `nv1_pfifo.xml`](https://github.com/envytools/envytools/blob/master/rnndb/fifo/nv1_pfifo.xml)
  supplements the donor with NV3 CACHE0/CACHE1 and runout EMPTY bits and
  DMA push access. These are additional idle checks, not an NV4 transport.
- [envytools `bus/pmc.xml`](https://github.com/envytools/envytools/blob/master/rnndb/bus/pmc.xml)
  identifies PMC enable bits 8 (PFIFO) and 12 (PGRAPH). Enable those units
  by read/modify/write, preserving every other bit, without the donor's
  global reset. Read-back and idle proof precede object setup.
- The owner's local Windows 98 Resource Kit `wrkc24_3.htm`, “Display Driver
  Overview”, describes the DIB engine handling unaccelerated operations.
  Ciuki's `swdib` and registry already provide this split. No book text is
  copied into the repository.

## Transport and ownership

Read-only PCI nomination issues no engine command and claims no resource.
The registry proves the active BAR1 framebuffer lease, generation and unique
PCI owner. The adapter rechecks that proof before activation. Only ordinary
32-bit memory BARs and enabled memory decode qualify; PCI bus mastering must
be disabled. It is never enabled by this driver. BAR extents must not alias
RAM, the kernel, one another or overflow. Activation claims BAR0, the NV3
instance aperture, and the last 32 KiB of physically reported VRAM. MMIO is
mapped UC in small private reusable windows; the active framebuffer mapping
remains fbdev's existing WC/UC mapping.

The engine uses PIO only. There is no pushbuffer, ring, host-memory address,
AGP/GART programming, IRQ handler or host DMA submission. The donor's local
VRAM translation context is bounded to the detected/firmware-agreed VRAM,
not blindly to 8 MiB. PRAMIN/RAMHT/RAMFC and cursor storage are reserved,
with no offscreen leasing in this first port. PFIFO DMA access is disabled
and read back before object binding. Interrupt enables stay zero. There are
no PMC resets, PFB configuration writes, timer, PLL, arbitration, CRTC timing
or mode changes. Unsupported scanout offset/pan/format/pitch refuses before
engine setup. The lead must review this NV3 PIO transport and reservation;
the directive enables activation subject to its mandatory DM9 comparisons.

All hardware mutation, including setup, runs inside `fbdev_accel_probe` with
exclusive drawing ownership. Before saving pixels, FIFO cache/runout and
PGRAPH idle are proved. Activation compares FILL, COPY in both overlap
directions and PATTERN against swdib, restores only after idle, and verifies
cursor storage, position, enable and final disable through register evidence.
A pass publishes the ops; otherwise same-boot quarantine persists.

Each poll has a 20 ms deadline and a frozen-clock iteration cap. Ordinary
presenter waits yield through the existing 1 ms pause hook, preserving drawing
ownership. A submission clears cached CPU readiness. Quarantine disables new
submissions and hides the cursor, but never itself proves engine idle. Only
bounded FIFO/PGRAPH idle plus cursor disable permits software drawing or
resource release. A failed proof retains the rejecting ops and claims.
Non-idempotent COPY errors are returned for caller reconstruction, never
replayed by the backend. No reset is used as an invented idle proof.

The durable keyed marker is `C:/system/settings/riva128.cfg`; its contents
include driver and PCI identity. An unreadable/nonempty marker prevents
activation. It is committed before any hardware mutation, and cleared only
after successful DM9. The old generic/Savage marker is neither cleared nor
reinterpreted; a nonempty or unreadable legacy marker also blocks activation
conservatively. Safe mode and exact `accel=0` cause no claims or marker writes.

FILL/COPY/SYNC, legacy PATTERN and monochrome cursor use the unchanged part A
hooks. MONO/INVERT/LINE require part C hooks/capability allocation, absent from
this base; no fabricated public ABI or capability numbers are introduced.
WRITE, KEYED, SCALED and ARGB are not donor paths in this selected closure.
