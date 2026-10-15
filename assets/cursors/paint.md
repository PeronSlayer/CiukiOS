# Paint cursors

`paint.h` contains original monochrome CiukiOS drawings under the MIT
license. No external artwork is included. Each raster occupies 32 by 32
pixels; mask bits select opaque pixels and image bits select the light
colour. The hotspot is relative to the raster's upper-left corner.

| Shape | Hotspot |
| --- | --- |
| Pencil | 0, 17 |
| Brush | 0, 15 |
| Bucket | 13, 17 |
| Picker | 0, 16 |
| Magnifier | 7, 5 |
| Eraser | 4, 4 |

Text, crosshair, precision and arrow use the generic rasters and hot spots
from `cursors.h`. The Paint capture helper shares those exact masks with
the desktop presenters.

Paint requests a shape through `DESK_SET_CURSOR`. Brush, airbrush and eraser
also paint a footprint over the visible image, using the exact zoomed stamp
geometry. The footprint belongs to the client surface and never changes image
pixels. The arrow fallback preserves the previous desktop raster; the generic
cursor inventory is maintained by F2-67.
