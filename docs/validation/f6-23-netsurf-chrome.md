# NetSurf chrome interaction and local layout

Research and implementation record for `f6-23-netsurf-chrome-interaction.md`.

Primary sources checked before implementation:

- [NetSurf source releases](https://www.netsurf-browser.org/downloads/source/):
  the pinned 3.11 full release is the comparison baseline. In its
  `frontends/framebuffer/fbtk/event.c:63`, `fbtk_click` chooses the widget at
  the current pointer location and gives input focus to an input handler.
  `fbtk/text.c:489` positions the writable field's caret;
  `gui.c:1164` creates a URL and navigates with history on Return.
  [Upstream gui.c](https://github.com/netsurf-browser/netsurf/blob/master/frontends/framebuffer/gui.c)
  was also checked online; line references here refer to the pinned release.
- [Framebuffer font documentation](https://github.com/netsurf-browser/netsurf/blob/master/docs/building-Framebuffer.md)
  describes cached FreeType faces and glyphs.
- [FreeType glyph retrieval](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html):
  advances and bearing deltas belong to the loaded glyph and depend on size,
  hinting and kerning. Cache whole-string SDK results without changing those
  semantics or substituting approximate advances.

Repository comparison: Ciuki uses deskui for chrome and fbtk for the page.
`nsfb_surface.c` offsets the framebuffer by `CIUKI_BROWSER_TOP` (36 pixels),
while desktop input remains relative to the entire client area. Chrome
motion was consumed before updating the page surface's pointer position.
This left the two input owners with different pointer positions; the
dispatcher now keeps both current before routing a button. Deskui URL focus
never set `toolbar_keys`, so TEXT and editing keys continued to reach the
page. That keyboard defect is reproduced by the host fixture. The historical
capture has no per-input records, so it cannot establish which toolbar
buttons were attempted or their state. Back/Forward have no history on the
welcome page, Stop is disabled after loading, Reload returns to the same
page, and Home was absent. The new action fixtures and bounded records make
these cases distinguishable; no hardware claim is made for the fix yet.

Decision: use one dispatcher for production and frontend tests. Update both
pointer positions on every motion, retain page drags until release, and let
deskui capture retain chrome drags. Entering the address field selects its
text; subsequent clicks can position its caret. Desktop TEXT supplies printable
characters, while Set-1 KEY supplies editing and shortcuts. Home uses the
configured homepage, falling back to `about:welcome`. Record bounded input
routes without recording URL text.

Layout decision: keep the existing four-face and glyph bounds. Cache bounded
whole-string width results by face style, size and exact bytes to avoid
repeated SDK measurement and its native mutex operations. The extra static
cache has 128 slots containing at most 256 text bytes each (about 35 KiB on
i686), with no allocations. Add font open and
measurement counters/times to distinguish those costs on hardware. Preserve
local-page exemption and all remote admission/deadline checks. The existing
32x host clock model excludes native disk and mutex costs; it cannot establish
that a physical T23 meets 300 ms. Exact test results and pending lead evidence
are in `build/codex/f6-23-report.md`.

`[netsurf] layout work` reports the first 16 HTML layout spans. `elapsed_us`
uses the existing budget clock (including any host test multiplier); the
font open and measurement times are unscaled monotonic wall times. Native
runs use multiplier 1. A width-cache hit still checks the remote deadline.
The welcome page uses one HTML layout pass in the host model. Its
`about:logo` redirects to `resource:netsurf.png`
(`content/fetchers/about/about.c:306`); this build disables PNG decoding
(`sdk/netsurf/build.py`, `FEATURES`), so PNG decoding cannot account for
the measured welcome HTML layout span. Preserve the page and image feature
selection; do not hide a slow layout by removing welcome content.

The lead probe is `scripts/test/netsurf_probe.py`. It uses the normal runner
lock and preflight, refuses a competing runner slot, uses a qcow2 overlay,
records the canonical image hash, and never builds. The lead supplies the
usual QEMU resource scope. Defaults match an unmodified Programs menu and
the US keyboard layout; installed Programs links require coordinate
overrides and an Italian layout requires `--layout it`. A completed capture
is diagnostic evidence, not an automatic network success verdict.
