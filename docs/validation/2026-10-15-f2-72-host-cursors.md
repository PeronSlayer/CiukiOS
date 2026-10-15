# F2-72: host expectations after the DMZ-White import

The X.Org [xcursorgen manual](https://xorg.freedesktop.org/archive/X11R7.5/doc/man/man1/xcursorgen.1.html)
defines each source entry as nominal size, hot spot, image and optional frame
delay. The [Xcursor manual](https://xorg.freedesktop.org/archive/X11R7.5/doc/man/man3/Xcursor.3.html)
distinguishes nominal size from actual dimensions and places the hot spot inside
the image. Animation requires multiple images; a busy shape alone does not imply
an animated upload.

Repository comparison: `scripts/cursors/import_theme.py` produces thirteen fixed
32 by 32 monochrome rasters for DMZ-White. `assets/cursors/cursors.h` now describes
the arrow with hot spot (10, 5) and occupied bounds 24 by 29; the previous arrow
had hot spot (0, 0) and bounds 11 by 17. Both presenters in
`apps/desktop/compositor.c` use the same masks and subtract the same hot spot.
The hardware request carries two arrays of 32 `uint32_t` rows, with position
submitted separately. The selected DMZ header contains no busy frame sequence;
the renderer uses its fixed busy raster.

Decision: keep the imported art and production implementation. Re-record only
cursor-dependent visual pins and replace the acceleration test's obsolete
374-pixel software-motion limit (twice 11 by 17) with a limit derived from the
selected arrow's bounds. Preserve the checks for zero scene painting and zero
uploads during hardware cursor motion, and all visible-frame equivalence,
hot-spot, clipping and fallback checks.

Verification isolated the header by force-including the pre-import raster data
from `647ef2a1^` into otherwise current host sources, without changing the
checkout. Every original scene, Paint and transient pin was reproduced. With
the imported header, each scene differs at 166 pixels in the rectangle
(788, 530)–(804, 556). Paint changes only cursor frames 00, 07–14 (nine pins);
its other 24 pins remain exact. Transients change only balloon, notification and
tooltip (three pins); their differences each occupy a 16 by 26 region. The other
nine transient pins remain exact. Header regeneration is byte-identical; all
thirteen generated rasters have in-range hot spots and bounds, and image bits
are subsets of their opaque masks. Each upload contains 256 bytes of mask/image
data. No importer or production-code correction was required.

On checkout `aac67e4d` with these test expectation corrections, the following
commands completed sequentially with exit status 0:

- `nice -n 19 make CAP= desktop`
- `nice -n 19 python3 -B -m unittest discover -s tests/host/desktop -t .`
  — 101 tests in 329.673 seconds, `OK`.
- `nice -n 19 python3 -B -m unittest discover -s tests/host -p test_desktop_gate.py`
  — 10 tests in 46.646 seconds, `OK`.

`git diff --check` passed. The acceleration harness retained software/hardware
visible-frame equivalence and zero uploads on hardware cursor motion.

Host verification and exact outputs are recorded in
`build/codex/f2-72-fix-report.md`. No image or guest qualification is implied.
