# Native desktop rendering

CiukiOS follows the painting contract used by classic window systems: changes
invalidate a screen area, and painting recreates that area from application
state in window order. The painter must never depend on pixels left by an
earlier frame. This is the useful part of the Windows 95/98 GDI model; copying
its internal driver code or claiming identical hardware behavior is neither
necessary nor possible on CiukiDOS's DOS/VGA stack.

Microsoft's GDI documentation describes [update regions][update],
[clipping to the visible and invalid region][clip], and repainting areas
exposed by a moved window [paint]. Windows 95/98 did not require a modern
whole-desktop compositing framebuffer for every GDI paint.

## Current pipeline

1. An application or shell action calls `ui_comp_damage` with a half-open
   rectangle. Requests are clipped to the screen and merged into the pending
   bounding rectangle. A full redraw is selected when the scene or video mode
   changes.
2. `ui_draw_scene` visits the background, top rail, desktop, windows, dialogs
   and overlays in display order. The draw primitives clip their writes to the
   current damage band. Input hit areas remain tied to this same scene.
3. VBE modes paint native packed pixels into a bounded scratch segment of at
   most 60 KiB. Allocation retries with smaller segments down to 4 KiB.
   Mode 12h paints 16-color indexes, one byte per pixel, into that segment.
   Text, icons and rectangles use the same scratch path while composition is
   active. The wallpaper and native DOS-window video surface remain VBE-only.
4. Presentation copies a completed band to the display. VBE uses its packed
   framebuffer path and optional verified video pages. Mode 12h rounds damage
   horizontally to eight pixels and converts complete index bytes into VGA's
   four planes without reading the old screen. The software pointer is hidden
   only where a presentation overlaps it, then restored.

Setup uses the same band composition approach. Its VGA fallback and VBE wizard
paint into private indexed bands, and VGA shares the desktop's four-plane
presenter. Progress updates repaint only their affected rectangle.

All drawing callbacks must use the scratch surface during composition. A
direct VGA write during a band paint will be covered by that band's final
copy; it is a correctness bug, even when it looks briefly right in QEMU.

## Remaining limits and validation

The pending damage is one bounding rectangle, not a list of disjoint regions.
Mode 12h has one visible video page, so successive completed bands can still
be seen during a large update. If even the 4 KiB scratch allocation fails, the
legacy direct painter keeps Desktop or Setup usable and may visibly redraw. None
of these paths guarantees tear-free presentation on every physical VGA BIOS.

The CPU tests check clipped paints and exact plane bytes, including untouched
neighbours. The full HDD QEMU test checks boot and desktop readiness; a HARD
profile screenshot checks the actual Mode 12h scene and window text. A Setup
regression boots disposable HDD images to check its VGA and VBE wizard. Physical
IBM T23 and Compaq E500 tests are still required before claiming that the
lampeggio is fixed on those machines.

[update]: https://learn.microsoft.com/en-us/windows/win32/gdi/invalidating-the-client-area
[clip]: https://learn.microsoft.com/en-us/windows/win32/gdi/window-regions
[paint]: https://learn.microsoft.com/en-us/windows/win32/learnwin32/painting-the-window
