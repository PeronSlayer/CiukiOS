# Ciuki cursors

`cursors.h` contains original project cursor drawings, released under the
MIT licence. The drawings use a white face and dark outline matching the
desktop controls. They contain no imported artwork. The approved Ciuki
portrait remains a separate asset and is unchanged.

Each cursor has a 32 by 32 canvas, a hot spot, an opaque mask and a white
foreground mask, with the leftmost pixel in bit 31. Width and height describe
the occupied bounds so ordinary arrow motion retains its small damage area.
The array order is the `desk_cursor_shape` order in the desktop protocol.
Both presenters consume these exact masks. The software presenter subtracts
the hot spot before clipping; the hardware presenter uses the same origin
and the existing negative-position clipping service.

The drawings cover arrow, hourglass, arrow with hourglass, text, forbidden,
horizontal, vertical, both diagonals, move, hand, crosshair and precision.
The masks are the source assets; no image decoder or generated runtime file
is needed.

## Retained cursor schemes

`CURSORS.DAT` is the retained 16 by 16 scheme store made by
`scripts/build_cursors.py`. It is independent of the VMM desktop raster set.
Its Tango, Classic and 3D Contrast drawings retain their Public Domain
notices. The Tango scheme follows the Tango Desktop Project style guidance;
the icon authors and release notice remain in `assets/icons/`.

The binary begins with `CIUKCUR1` (eight bytes), a 16-bit theme count and
a 16-bit entry size (214 bytes). Each entry has a 16-byte terminated theme
name and three cursors: arrow, I-beam and forbidden. A cursor stores sixteen
16-bit outline-mask rows, sixteen 16-bit white-mask rows and one byte for
each hot-spot coordinate. The new MIT raster source does not change the
licence of this retained store.
