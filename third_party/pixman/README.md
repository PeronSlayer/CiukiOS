# pixman region code (X11 regions)

The desktop compositor (`apps/desktop/compositor.c`) keeps its damage and
clip regions with the region algorithms of the X11 sample server as
maintained in pixman (f2-95, lead amendment of 2026-10-22).

F2-98b also uses this exact region implementation in NetSurf's private
surface transport (`sdk/netsurf/port/surface.c`) to retain separate invalid
rectangles. The compositor keeps its existing fixed pool; the browser uses
ordinary allocation and releases the region after publication or resize.
Its build fingerprints these files and installs `COPYING` as `pixman.txt`.

Upstream release: pixman **0.46.4**, archive
<https://www.cairographics.org/releases/pixman-0.46.4.tar.xz>

| Item | SHA-256 |
| --- | --- |
| `pixman-0.46.4.tar.xz` (archive) | `a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239` |
| `pixman-region.c` (unmodified) | `d1b9ed3adfbc7565eda1c18e00490bc705b0d8fbe595c869827dfbf0487429e4` |
| `pixman-region32.c` (unmodified) | `cac3e594acfd346b5d8bf0a842d54a48d70b424454959e90bfb6b7882bcbecdf` |
| `COPYING` (unmodified) | `fac9270f0987b96ff4533fca3548c633e02083cbba4a0172a3b149b2e4019793` |

The archive's SHA-512 matched upstream's published
`pixman-0.46.4.tar.xz.sha512`
(`83b133e7969ba34f883f4e08dcc5d388c4397f43ce836c191c05945fe77c16ff501d531600780c12678a0d08105828a6bdeff2156b63f9c1a84087bc7f40ae9f`)
when it was downloaded on 2026-10-22.

Files:

- `pixman-region.c`, `pixman-region32.c`: upstream, byte for byte. The
  region code descends from the X11 sample server's `miregion.c` (The Open
  Group, Digital Equipment Corporation, Keith Packard; notices in the file).
- `COPYING`: upstream licence and copyright list (MIT and the X11 notices).
- `pixman-private.h`: **not upstream**. A CiukiOS header that supplies only
  what the two files above need: the 32-bit region types copied from
  upstream `pixman.h`, the assertion macros of upstream `pixman-private.h`,
  the `_pixman_log_error` message of upstream `pixman-utils.c`, `MIN`/`MAX`,
  the little-endian `SCREEN_SHIFT_*` macros and stand-ins for the a1-image
  accessors of `pixman_region32_init_from_image`. It gives every region
  function internal linkage, so only the functions the compositor calls are
  compiled: the floating-point entry points and the bitmap converter are
  never emitted and the integer-only compositor audit of
  `apps/desktop/build_desktop.py` still applies.
  Upstream defines `pixman_region32_print` without `PIXMAN_EXPORT`; a prior
  `static` declaration in this header gives it internal linkage as well
  (C11 6.2.2p4). Without it, NetSurf (which links libdeskui with the
  compositor's copy and its own surface copy) failed with a duplicate symbol
  (lead fix 2026-10-23 after the f2-98a/f2-98b merges).

Not taken this round: pixman's image, compositing and fast-path code
(`pixman-fast-path.c`, `pixman-mmx.c`, `pixman-sse2.c` and the
implementation dispatch). See `docs/design/desktop-rendering-architecture.md`
(f2-95) for the reasons and the measured gap.
