"""fx-24 rendered-surface checks for fixtures/fx24/styled.html.

The fixture places every object at an absolute position (styled.css), so
the checks are pixel samples of the XRGB8888 capture that do not depend on
font metrics. Each expectation names what upstream NetSurf's framebuffer
front end shows there: author CSS from a linked stylesheet, an @import, a
style element and a style attribute; JPEG, PNG (with alpha), GIF, WebP and
SVG images; a tiled background image; a JPEG larger than the bitmap
admission. On the code before fx-24 (no PNG/JPEG/WebP/SVG handlers, no
path plotter) the image samples show the page background instead."""
# SPDX-License-Identifier: MIT
PAGE = (0xff, 0xff, 0xe0)
SAMPLES = [
    # name, x, y, expected RGB, tolerance (JPEG is lossy)
    ("linked stylesheet", 50, 30, (0x20, 0x50, 0xa0), 0),
    ("@import stylesheet", 50, 70, (0x00, 0xa0, 0x40), 0),
    ("style element", 50, 110, (0xc0, 0x00, 0x00), 0),
    ("style attribute", 50, 150, (0xe0, 0xa0, 0x00), 0),
    ("JPEG scaled 40->80, top-left quadrant", 140, 40, (220, 30, 30), 40),
    ("JPEG scaled 40->80, top-right quadrant", 180, 40, (30, 170, 40), 40),
    ("JPEG scaled 40->80, bottom-left quadrant", 140, 80, (40, 60, 220), 40),
    ("JPEG scaled 40->80, bottom-right quadrant", 180, 80, (240, 200, 20), 40),
    ("PNG opaque disc", 252, 52, (192, 0, 192), 2),
    ("PNG transparent corner shows the page", 222, 22, PAGE, 0),
    ("GIF left half", 326, 36, (0, 96, 96), 0),
    ("GIF right half", 346, 36, (255, 128, 0), 0),
    ("WebP stripe 1", 424, 36, (200, 0, 0), 0),
    ("WebP stripe 2", 432, 36, (0, 0, 200), 0),
    ("WebP stripe 3", 440, 36, (0, 160, 0), 0),
    ("WebP stripe 4", 448, 36, (30, 30, 30), 0),
    ("SVG filled rect", 144, 164, (0x20, 0x40, 0xc0), 0),
    ("SVG Bezier circle", 188, 164, (0xd0, 0x20, 0x20), 0),
    ("SVG stroked line", 170, 200, (0x10, 0x90, 0x30), 0),
    ("SVG ring", 178, 212, (0x20, 0x20, 0x20), 0),
    ("SVG ring hole (non-zero)", 192, 221, PAGE, 0),
    ("SVG transparent field", 168, 180, PAGE, 0),
    ("tiled background dark", 262, 144, (0, 0, 0), 0),
    ("tiled background light", 266, 144, (255, 255, 255), 0),
    ("tiled background repeats", 262 + 8 * 20, 144, (0, 0, 0), 0),
    ("tiled background repeats light", 266 + 8 * 20, 144, (255, 255, 255), 0),
    ("repeat-x only: below the tile the background colour", 262, 152, (255, 255, 255), 0),
    ("viewBox-only SVG keeps its aspect ratio (80x20)", 24, 204, (0x80, 0x40, 0xc0), 0),
    ("viewBox-only SVG inner rect", 60, 210, (255, 255, 255), 0),
    ("viewBox-only SVG bottom edge", 24, 218, (0x80, 0x40, 0xc0), 0),
    ("viewBox-only SVG ends at its height", 24, 224, PAGE, 0),
    ("large JPEG at a DCT scale, top-left", 300, 210, (220, 30, 30), 40),
    ("large JPEG at a DCT scale, top-right", 380, 210, (30, 170, 40), 40),
    ("large JPEG at a DCT scale, bottom-left", 300, 270, (40, 60, 220), 40),
    ("large JPEG at a DCT scale, bottom-right", 380, 270, (240, 200, 20), 40),
]

def pixel(raw, width, x, y):
    b, g, r, _ = raw[(y * width + x) * 4:(y * width + x) * 4 + 4]
    return (r, g, b)

def failures(raw, width):
    """Names of the samples that do not show what they should."""
    bad = []
    for name, x, y, want, tolerance in SAMPLES:
        got = pixel(raw, width, x, y)
        if any(abs(a - b) > tolerance for a, b in zip(got, want)):
            bad.append(f"{name} at ({x},{y}): {got} != {want} +-{tolerance}")
    return bad
