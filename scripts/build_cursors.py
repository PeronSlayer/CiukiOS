#!/usr/bin/env python3
"""Build CURSORS.DAT containing cursor schemes for CiukiOS."""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]

def art_to_words(art):
    black = []
    white = []
    for line in art:
        b_word = 0
        w_word = 0
        for i, ch in enumerate(line):
            if ch == "#":
                b_word |= (1 << (15 - i))
            elif ch == ".":
                b_word |= (1 << (15 - i))
                w_word |= (1 << (15 - i))
        black.append(b_word)
        white.append(w_word)
    assert len(black) == 16 and len(white) == 16
    return black, white

# --- THEME 0: TANGO (Default) ---
tango_arrow_art = [
    "#               ",
    "##              ",
    "#.#             ",
    "#..#            ",
    "#...#           ",
    "#....#          ",
    "#.....#         ",
    "#......#        ",
    "#.......#       ",
    "#.....####      ",
    "#..#..#         ",
    "#.# #..#        ",
    "##  #..#        ",
    "#    #..#       ",
    "     #..#       ",
    "      ##        ",
]

tango_ibeam_art = [
    "  ######        ",
    " #......#       ",
    "  ##..##        ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "  ##..##        ",
    " #......#       ",
    "  ######        ",
]

tango_no_art = [
    "     ######     ",
    "   ##......##   ",
    "  #...##.....#  ",
    " #...####.....# ",
    " #..##..##....# ",
    "#..##....##....#",
    "#...##....##...#",
    "#....##....##..#",
    "#....##....##..#",
    "#...##....##...#",
    "#..##....##....#",
    " #..##..##....# ",
    " #...####.....# ",
    "  #...##.....#  ",
    "   ##......##   ",
    "     ######     ",
]

# --- THEME 1: CLASSIC 95 ---
classic_arrow_art = [
    "#               ",
    "##              ",
    "#.#             ",
    "#..#            ",
    "#...#           ",
    "#....#          ",
    "#.....#         ",
    "#......#        ",
    "#.......#       ",
    "#.....##        ",
    "#..##..#        ",
    "#.#  #..#       ",
    "##    #..#      ",
    "#      #..#     ",
    "        ##      ",
    "                ",
]

classic_ibeam_art = [
    "   #####        ",
    "  #.....#       ",
    "   ##.##        ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "    #.#         ",
    "   ##.##        ",
    "  #.....#       ",
    "   #####        ",
]

classic_no_art = [
    "     ######     ",
    "   ##......##   ",
    "  #...##.....#  ",
    " #...#..#.....# ",
    " #..#....#....# ",
    "#..#......#....#",
    "#..##......#...#",
    "#...##......#..#",
    "#....##......#.#",
    "#.....##.....#.#",
    "#......##....#.#",
    " #......##..#.# ",
    " #.......###..# ",
    "  #..........#  ",
    "   ##......##   ",
    "     ######     ",
]

# --- THEME 2: HIGH CONTRAST / 3D ---
contrast_arrow_art = [
    "###             ",
    "####            ",
    "##.##           ",
    "##..##          ",
    "##...##         ",
    "##....##        ",
    "##.....##       ",
    "##......##      ",
    "##.......##     ",
    "##......####    ",
    "##..##..##      ",
    "##.## ##..##    ",
    "####   ##..##   ",
    "###     ##..##  ",
    "##       ####   ",
    "                ",
]

contrast_ibeam_art = [
    " ########       ",
    "##......##      ",
    " ###..###       ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    "   #..#         ",
    " ###..###       ",
    "##......##      ",
    " ########       ",
]

contrast_no_art = [
    "    ########    ",
    "  ###......###  ",
    " ##...###....## ",
    "##...#####....##",
    "##..###..###..##",
    "##.###....###.##",
    "##..###....###.#",
    "##...###....####",
    "####....###...##",
    "#.###....###..##",
    "##.###....###.##",
    "##..###..###..##",
    "##....#####...##",
    " ##....###...## ",
    "  ###......###  ",
    "    ########    ",
]

THEMES = [
    {
        "name": "Tango",
        "cursors": [
            (tango_arrow_art, 0, 0),
            (tango_ibeam_art, 4, 7),
            (tango_no_art, 7, 7),
        ]
    },
    {
        "name": "Classic",
        "cursors": [
            (classic_arrow_art, 0, 0),
            (classic_ibeam_art, 4, 7),
            (classic_no_art, 7, 7),
        ]
    },
    {
        "name": "3D Contrast",
        "cursors": [
            (contrast_arrow_art, 0, 0),
            (contrast_ibeam_art, 4, 7),
            (contrast_no_art, 7, 7),
        ]
    },
]

def resize_art(dx, dy):
    """Deterministic two-headed arrow with a black one-pixel outline.

    Keep the functional resize cursors identical across themes so a thin
    edge remains recognisable against either a bright or dark wallpaper.
    Existing theme artwork is preserved verbatim.
    """
    face = {(7 + step * dx, 7 + step * dy) for step in range(-5, 6)}
    back, width = (2, 2) if dx and dy else (3, 3)
    for sign in (-1, 1):
        tx, ty = 7 + sign * 5 * dx, 7 + sign * 5 * dy
        bx, by = tx - sign * back * dx, ty - sign * back * dy
        points = [(tx, ty), (bx - width * dy, by + width * dx),
                  (bx + width * dy, by - width * dx)]
        for y in range(16):
            for x in range(16):
                crosses = [(b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])
                           for a, b in zip(points, points[1:] + points[:1])]
                if min(crosses) >= 0 or max(crosses) <= 0:
                    face.add((x, y))
    face = {(x, y) for x, y in face if 1 <= x <= 14 and 1 <= y <= 14}
    outline = {(x + ox, y + oy) for x, y in face
               for ox in (-1, 0, 1) for oy in (-1, 0, 1)
               if 0 <= x + ox < 16 and 0 <= y + oy < 16}
    return ["".join("." if (x, y) in face else "#" if (x, y) in outline else " "
                    for x in range(16)) for y in range(16)]


RESIZE_CURSORS = [(resize_art(dx, dy), 7, 7)
                  for dx, dy in ((1, 0), (0, 1), (1, 1), (1, -1))]
CURSOR_COUNT = 7

def build():
    out_dir = ROOT / "assets" / "cursors"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "CURSORS.DAT"
    
    # CIUKCUR2 adds the four sizing cursors after the original three.
    theme_entry_size = 16 + CURSOR_COUNT * 66
    header = struct.pack("<8s2H", b"CIUKCUR2", len(THEMES), theme_entry_size)
    
    body = bytearray()
    for t in THEMES:
        name_bytes = t["name"].encode("ascii")[:15].ljust(16, b"\0")
        body.extend(name_bytes)
        for art, hx, hy in t["cursors"] + RESIZE_CURSORS:
            b_words, w_words = art_to_words(art)
            for w in b_words:
                body.extend(struct.pack("<H", w))
            for w in w_words:
                body.extend(struct.pack("<H", w))
            body.extend(struct.pack("2B", hx, hy))
            
    data = header + bytes(body)
    out_file.write_bytes(data)
    print(f"[build-cursors] wrote {len(data)} bytes to {out_file}")

if __name__ == "__main__":
    build()
