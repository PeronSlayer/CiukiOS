# 101-b: DOS frame presentation

Research and implementation record, 2026-10-25. Directive:
`docs/directives/101-b-dos-video-accel.md` (read completely; the supplied
revision has no appended amendments or lead notes).

## Primary sources and implementation decisions

- Windows 98 Resource Kit, chapter 24, “Display Driver Overview”, local
  read-only `build/rskt/wrkc24_3.htm`, paragraphs at lines 202–214: the DIB
  engine handles memory drawing and on-screen operations that the minidriver
  does not accelerate. Keep system-surface rendering in software and submit
  screen copies through the existing generic display interface.
- [Raymond Chen, Microsoft, Windows 95 grabbers](https://devblogs.microsoft.com/oldnewthing/20231127-00/?p=109067):
  the VDD virtualizes the adapter; the grabber compares changes and produces
  incremental window drawing. This supports retained row comparison and
  exact damage. It is the Windows 9x model, not NTVDM.
- [QEMU 10.1 VGA implementation](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c),
  `update_palette256` and `vga_draw_graphic`: palette processing happens at
  refresh, and dirty-page scanlines form update runs. CiukiOS already batches
  DAC writes in fx-27. Refine palette damage at presentation, rather than
  converting after individual DAC writes.
- [X.Org Savage XAA hooks](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c)
  and [engine implementation](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c):
  copy, fill, image write and monochrome expansion have separate hooks. No
  stretch hook is established by these sources. In this repository no native
  driver advertises `CIUKI_CMD_SCALED`; only swdib implements it. Do not
  claim native stretch or introduce unqualified overlay/3D programming.
- [X.Org EXA interface](https://raw.githubusercontent.com/XQuartz/xorg-server/master/exa/exa.h):
  copy direction and CPU/engine synchronization are explicit. Reuse the
  existing synchronous display COPY and its failure/reconstruction path.
  A duplicate row can use the preceding row just successfully presented in
  the same call, including the first frame; it need not trust old scanout.

## Baseline path and measurement boundary

Windowed mode 13h and Mode X: trapped VGA ports → dirty PTE harvest and
retained indexed-row comparison → palette expansion/integer scaling into
640×480 XRGB → shared VM surface → dosbox Fit/100%/125% viewport → application
surface → desktop composition → generic display present/native copies.
Mode 13h (320×200) converts 64,000 source bytes, emits 400 image rows plus
80 border rows on initialization, and writes 1,024,000 image bytes on a full
subsequent frame. Fit paints 640×452 = 289,280 application pixels. Mode X
320×240 converts 76,800 source bytes and writes 480 image rows (1,228,800
XRGB bytes) for a changed full frame. Page flips already compare displayed
bytes and need not redraw equal pages.
VBE 640×480×8 converts 307,200 source bytes and writes 1,228,800 XRGB bytes
for a full frame; page dirt limits partial updates. Palette-only changes
currently force conversion and destination comparison even for unused colours.

Full-screen VGA already grants the adapter aperture (and native VGA ports)
to the owning VM. VBE retains its pinned private RAM and flushes dirty pages
through the provider's `write` hook. A full VBE 640×480×8 frame therefore
copies 307,200 bytes (75 pages) to the native LFB, without RGB conversion.
Neither path performs window conversion, Fit, application copy or compositor
present while that lease is active. Transition save/restore and watchdog
work must not be counted as a per-frame upload. Direct VBE mapping needs a
new provider/physical-extent and revocation contract; it is remaining work.

The directive's QEMU bank (108,879 VGA exits / 334,859 us) is 3.0755 us per
exit on that QEMU run, not a measurement of the PIII-M. The earlier T23
records and model in `fx-27-windowed-dos-graphics.md` remain the only supplied
hardware timing basis. New host timings and deterministic work counts are
reported separately in `build/codex/101-dosvid-report.md`. Physical per-stage
times and true native stretch qualification require the lead's evidence.

## Implemented behavior

The VGA presenter computes a 256-bit palette delta once per sample. An
indexed row is damaged only when its indexes change, or it uses a changed
colour through the sampled DAC mask. A palette-proven RGB difference skips
the destination-surface comparison. Geometry, blanking, source-layout and
mask changes retain the conservative redraw path.

VBE8 reuses the existing 640×480 retained index array for the sampled
window image, including 800-pixel input rows. It compares page-dirty rows
before converting them, skips equal rewrites, and proves changed rows to
the surface writer. It allocates no additional VM frame. VBE16 ignores DAC
changes, since RGB565 pixels do not depend on the palette. BIOS 4F09 palette
updates keep the retained frame valid and respect the presentation deadline.

The dosbox viewport copies Fit/100% rows directly, including clamped
horizontal scrolling. At 125% it computes 640 column coordinates once per
paint. Text selection preserves the scalar cell-coordinate rule. The row
damage protocol and overlays retain their existing behavior.

The desktop now accelerates solid fills and repeated scanline copies in
its first frame. A COPY reads only a source completed earlier in that call;
it never depends on old scanout. A private `restore_cpu_frame` flag preserves
the complete CPU repaint required after a display-lease generation change.
Errors still retain damage and reconstruct the frame from the client surface.
This accelerates presentation of scaled frames through native COPY/FILL;
it does not introduce or advertise a native SCALED command.

## Host evidence

Baseline production files are from `377e660f5783c74122fa6da877c86dfb3ae3aa26`.
The same new regressions were compiled against that baseline and the change.
Eighteen deterministic VM budgets pass in the changed test body; sixteen
fail on the baseline. Equal Mode X page flips and the already bounded
mode-13h conversion are unchanged controls. Retained regression frames are
checked against a forced redraw. Existing fx-27, full VM and VGA assertions also
complete, but LeakSanitizer cannot complete its ptrace-dependent exit check
in the implementer's sandbox. These executions return failure, not PASS.

The synthetic 800×600 first desktop frame with a 640×480 Fit DOS window
uses 480,000 CPU-uploaded pixels before and 205,600 after, with 273 native
copies and 15 fills. Both paths match the CPU reference, as does recovery
after a submitted COPY fails. The display-model/command suites, the full
desktop acceleration suite, compositor goldens, instruction-emulator vectors
and kernel stack suite pass. The changed kernel units compile and dosbox
and desktop link as i686 ELFs with production flags and `-Werror`.

The report contains the precise verification table, sanitizer limitations,
host timing samples and the remaining native-stretch/direct-VBE decisions.
No physical or QEMU validation, complete kernel link or image build is claimed.
