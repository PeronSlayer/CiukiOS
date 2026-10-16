# F3-37 additional native full-screen modes

Source research and host implementation: 2026-10-16, branch `wt/f3-37`.
The [design amendment](../design/supersavage-fullscreen.md#f3-37-mode-inventory-and-guest-vbe-amendment-2026-10-16)
records the mode inventory, primary sources, the IBM INF's lack of a mode
list and the virtual VBE framebuffer contract. F3-36 recovery remains
authoritative, including strict writable-register readback and the mediated
desktop VBE fallback.

## Host coverage

- Production SuperSavage programming: all existing BIOS VGA presets;
  640x480/800x600 at 8/16 bpp, PLL frequency, CR67 mobile colour depth,
  pitch, CRTC timing, unchanged panel/MCLK, exact extended/VGA/palette/plane
  restoration, and the F3-36 derived-panel-register tests.
- Provider lifecycle: packed BAR1 writes require the framebuffer's quiesced
  generation, reject overruns, and revoke the temporary alias before restore;
  failed mapping/ownership rolls back without scanout programming.
- Production VM: 16 chain-4/unchained 320/360 x200/240/400/480 variants and
  existing planar VGA presets enter, produce nonblack model pixels, observe
  a modeled guest-store dirty bit, capture and return. VBE queries/selection,
  buffer bounds, unsupported modes, guest RAM transport and DPMI token
  resolution are exercised for the four supported modes. Every mode gets a
  first linear-store observation, exact RAM PTE retention, returned window
  frame and a surfaced restore failure. VM/client cleanup returns resources.
- Display service: each packed mode passes both native return and native
  failure followed by the mediated desktop VBE fallback, including grant
  return and the first completed desktop frame.
- Both existing `vm-fullscreen` QEMU cases now require eight frozen VGA
  raster cycles and four explicit VBE refusals, in addition to the existing
  live DOOM, complete screen equality and cleanup evidence. Host parser
  controls reject missing or falsified per-mode rows. These new QEMU cases
  have not been run by the implementer.

Commands, exact summaries and limitations are in
`build/codex/f3-37-report.md`. Host checks run sequentially at nice level 19.
No image, QEMU, systemd scope, commit or diary operation is part of this work.

## Qualification still required

The lead must build the canonical integrated image, retain its SHA-256 and
run `vm-fullscreen` on standard VGA and Cirrus. The existing F3-29 standard
VGA readback failure remains a separate finding; no masks were relaxed.
Frozen raster cycles prove transport and return, not a newly executing
guest frame. QEMU cannot qualify SuperSavage native VBE because its profiles
do not emulate that adapter; the new VBE rows must report 014Fh there.

On the T23, test the Mode X rasters, planar 12h, and a DOS VBE client using
101h/103h/111h/114h with a linear framebuffer. Verify actual colours, pitch,
panel centering/expansion, first-frame records, Alt+Enter/Alt+Tab, clean
exit, fault/watchdog return and visible desktop repaint. Measure copying
throughput and the existing restore/lease/first-desktop-frame budgets;
host tests do not establish hardware frame rate. VBE mode changes while
full-screen return through the worker transaction and continue windowed in
the selected virtual mode; Alt+Enter can request native entry again.
Banked VBE, 15 bpp, custom refresh and additional resolutions are unavailable.
