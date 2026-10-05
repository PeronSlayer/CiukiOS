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

def build():
    out_dir = ROOT / "assets" / "cursors"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "CURSORS.DAT"
    
    # Header: Magic CIUKCUR1 (8 bytes), count (u16), theme_bytes (u16)
    theme_entry_size = 16 + 3 * 66 # 214 bytes
    header = struct.pack("<8s2H", b"CIUKCUR1", len(THEMES), theme_entry_size)
    
    body = bytearray()
    for t in THEMES:
        name_bytes = t["name"].encode("ascii")[:15].ljust(16, b"\0")
        body.extend(name_bytes)
        for art, hx, hy in t["cursors"]:
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
