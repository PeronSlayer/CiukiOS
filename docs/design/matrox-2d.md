# Matrox G200–G550 native 2D port (f2-110)

Research and implementation date: 2026-10-23 (`date -Is` checked).
This implements the lead's f2-110 directive and display-driver-model Amendment
1, behind the part A registry. VBE owns modes, clocks, panels and VRAM sizing.

## Primary sources and decisions

- Linux **v6.12**, [`matroxfb_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_accel.c):
  `matrox_cfbX_init`, `matrox_accel_clear`, `matrox_accel_bmove`, and
  `matroxfb_1bpp_imageblit` provide the register recipes. Import the native
  engine fragments, with their original notice, rather than hosting fbdev.
  Replace infinite polling with time and iteration bounds. Reserve FIFO space
  for every write, including the two MACCESS/PITCH writes the donor sometimes
  omits from its FIFO accounting. FILL uses only ROP_COPY. COPY traverses both
  axes backwards for lower/right destinations; no failed COPY is replayed.
- Linux **v6.12**, [`matroxfb_base.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_base.h)
  supplies registers, FIFO count and engine-busy definitions;
  [`matroxfb_base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_base.c)
  supplies BAR selection, pitch alignment and the PCI/revision table. Only
  102b:0520, 0521, 0525 and 2527 match. On 0525, revision <=80h is G400,
  higher is G450. All four use framebuffer BAR0, control BAR1 (16 KiB).
  Keep the stricter 32-pixel pitch alignment for G400-class 32 bpp, and
  64-pixel alignment at 24 bpp. Accept canonical RGB555/565, BGR888 and
  XRGB8888 only; other valid VBE layouts use the software engine.
- Linux **v6.12**, [`matroxfb_DAC1064.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_DAC1064.c)
  and [`matroxfb_DAC1064.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_DAC1064.h)
  supply cursor-disable and DAC definitions. This release no longer provides
  cursor loading; do not describe it as a complete cursor donor.
- Matrox, **MGA-G200 Specification**, 10582-MS-0301, November 30 1998,
  [vendor manual preserved by VGA Museum](https://old.vgamuseum.info/images/stories/doc/matrox/mga-g200_spec.pdf):
  pp. 3-108, 3-155–156, 3-304, 3-319–323 and 4-72 define FIFO/idle and
  cursor control. Cursor storage is 1 KiB aligned below 16 MiB, with two
  64-bit slices per row. X-Windows mode maps mask/image to transparent,
  background and foreground. Positions encode the bottom right of a 64×64
  plane, so the legacy top-left coordinates receive +64. Update position
  outside VSYNC; fully offscreen positions use zero. Cursor SET hides first
  and leaves hidden. Native pixel colours are expanded to the DAC's RGB8.
- Matrox, [G400 Specification](https://old.vgamuseum.info/images/stories/doc/matrox/mga-g400_spec.pdf),
  p. 3-141: the G400 FIFO has **16** entries, unlike G200's 64. Idle proof
  uses the common `bempty` flag (bit 9), never a hard-coded free count.
  Mono configuration and the launch/data packet reserve separately, each
  fitting within 16 entries. G400/G450/G550 fixtures model that smaller FIFO.
  Page 3-77 defines CRTC2 enable. A running second head makes cursor/offscreen
  ownership ambiguous, so this port reserves no cursor storage in that case.
  [Haiku r1beta5 `Cursor.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/matrox/Cursor.c)
  also documents that the native cursor belongs to CRTC1; no Haiku code is
  imported. Desktop software cursor ownership remains available.
- The owner's read-only Windows 98 Resource Kit, `wrkc28_8.htm:73–84`
  and `wrkc26_5.htm:12–27`, confirms the DIB/minidriver split and independent
  cursor/blit disable policy. Use swdib for unimplemented calls, only after
  completion proof. No Resource Kit content is imported.

Compared with the repository, part A already provides PCI/active-LFB lease
ownership, safe-mode/accel=0 policy, serialized activation probes, generic
DM9 FILL/COPY readback and UI identity records. The port supplies its own
MMIO/cursor claims, durable attempt hooks, register tests and cached guard.
It adds no public ABI records or SDK headers. The missing family and optional
command contracts were read from the lead checkout, including their lead
decisions; the delivered family row is in display-driver-model.md.

## Safety and optional commands

Probe examines PCI metadata only. Start rechecks the selected owner and mode,
then claims uncached MMIO and optionally a disjoint cursor page within the
firmware-reported VRAM. It never enables PCI bus mastering, DMA, AGP or a PLL.
All engine setup and drawing probes run under fbdev_accel_probe. Existing
framebuffer pixels are saved/restored only after idle proof. No destructive
memory sizing or undocumented engine reset is attempted.

Polling has a 20 ms deadline, 1 ms presenter slices and a frozen-clock cap.
A packet is submitted only with enough free FIFO entries, so an engine
waiting for work cannot fill the bus with retries. Quarantine disables new
submissions and hides the cursor; CPU access is cached false until the FIFO
is empty, engine busy has cleared and cursor disable is read back. Failure
retains claims and mappings and rejects CPU access. Stop revokes mappings
and releases claims only after this proof. Quarantine is permanent this boot.

Part A exposes FILL/COPY/SYNC and the legacy cursor hooks. A private bounded
mono-expansion entry point is prepared and host-tested, without allocating a
public opcode or capability ahead of f2-101d. Non-mono uploads, unsupported
ROP/format/geometry and unavailable cursor storage are clean refusals.
Optional native bits need the lead's physical DM9 qualification; host models
are evidence of sequences and pixels, not evidence that silicon executed.

## Acceptance boundary

Separate physical acceptance is required for G200 and G400/G450/G550.
The lead runs the canonical build and hardware checks. This worktree runs
host fixtures only; commands and exact results are in
`build/codex/f2-110-report.md`.

Repository integration correction: `hardware_check.c` originally skipped an
adapter without a private configuration-sweep hook before reaching the
generic DM9 comparison. DM1/DM9 make that sweep optional. The H page now
dispatches directly to fbdev_accel_diagnostic for such adapters, preserving
the existing sweep path for drivers that provide it. Refused native work or
a missing FILL/COPY coverage mask cannot report PASS.
