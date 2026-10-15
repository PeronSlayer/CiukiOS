#!/usr/bin/env python3
"""Import a 32px cursor theme into the desktop's deterministic monochrome masks."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[2]
DMZ = ROOT / "build/downloads/dmz-cursor-theme/dmz-cursor-theme-0.4.5"
HACK = ROOT / "build/downloads/hackneyed/hackneyed-x11-cursors-master"
DESKTOP = ["arrow", "busy", "arrow_busy", "text", "forbidden", "resize_ew", "resize_ns", "resize_nwse", "resize_nesw", "move", "hand", "crosshair", "precision"]
PAINT = ["pencil", "brush", "fill", "pick", "zoom", "eraser"]
DMZ_MAP = {"arrow":"left_ptr", "busy":"watch", "arrow_busy":"left_ptr_watch", "text":"xterm", "forbidden":"crossed_circle", "resize_ew":"sb_h_double_arrow", "resize_ns":"sb_v_double_arrow", "resize_nwse":"bd_double_arrow", "resize_nesw":"fd_double_arrow", "move":"move", "hand":"hand2", "crosshair":"crosshair", "precision":"tcross", "pencil":"pencil", "brush":"pencil", "fill":"color-picker", "pick":"color-picker", "zoom":"plus", "eraser":"crossed_circle"}
HACK_MAP = {"arrow":"default", "busy":"progress-1", "arrow_busy":"progress-1", "text":"text", "forbidden":"not-allowed", "resize_ew":"ew-resize", "resize_ns":"ns-resize", "resize_nwse":"nwse-resize", "resize_nesw":"nesw-resize", "move":"move", "hand":"pointer", "crosshair":"crosshair", "precision":"crosshair", "pencil":"pencil", "brush":"pencil", "fill":"color-picker", "pick":"color-picker", "zoom":"zoom-in", "eraser":"not-allowed"}
HACK_SVG = {"default":"default", "progress-1":"progress-1", "text":"pointer", "not-allowed":"no-drop", "ew-resize":"exchange", "ns-resize":"exchange", "nwse-resize":"exchange", "nesw-resize":"exchange", "move":"exchange", "pointer":"pointer", "crosshair":"zoom", "pencil":"pencil", "color-picker":"color-picker", "zoom-in":"zoom-in"}

def hotspot(path: pathlib.Path, preferred=32) -> tuple[int, int, int]:
    entries=[line.split() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    row=min(entries,key=lambda e:abs(int(e[0])-preferred))
    return int(row[1]), int(row[2]), int(row[0])

def raster(theme: str, name: str, frame: int = 1) -> tuple[Image.Image, tuple[int,int]]:
    if theme.startswith("dmz"):
        folder = "DMZ-White" if theme == "dmz-white" else "DMZ-Black"
        base = DMZ / folder / "pngs"
        png = base / "32x32" / f"{DMZ_MAP[name]}.png"
        if not png.exists():
            png = base / "24x24" / f"{DMZ_MAP[name]}.png"
        hx,hy,hs = hotspot(base / f"{DMZ_MAP[name]}.in")
        point=(hx,hy)
        im = Image.open(png).convert("RGBA").resize((32,32), Image.Resampling.NEAREST)
        if im.size != (32,32): raise ValueError(f"unexpected cursor size: {png}")
        point = (round(point[0]*32/hs), round(point[1]*32/hs))
        return im, point
    base = HACK / "theme"
    hotspot_name = "wait" if name in ("busy", "arrow_busy") else HACK_MAP[name]
    in_file = next((base / str(size) / f"{hotspot_name}.in" for size in (24,36,48,60,72,84,96) if (base / str(size) / f"{hotspot_name}.in").exists()),None)
    svg_name = f"progress-{frame}" if name in ("busy", "arrow_busy") else HACK_SVG[HACK_MAP[name]]
    svg = base / "right-handed-light" / f"{svg_name}.svg"
    if in_file is None or not svg.exists(): raise FileNotFoundError(f"theme cursor mapping is incomplete: {name} ({svg_name})")
    x,y,hs = hotspot(in_file)
    with tempfile.TemporaryDirectory(prefix="ciuki-cursor-") as td:
        out = pathlib.Path(td) / "cursor.png"
        subprocess.run(["inkscape", str(svg), "--export-width=32", "--export-height=32", f"--export-filename={out}"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        im = Image.open(out).convert("RGBA")
    return im, (round(x*32/hs), round(y*32/hs))

def pack(im: Image.Image, point: tuple[int,int]):
    px = im.convert("RGBA").load(); mask=[]; image=[]; bounds=[32,32,-1,-1]
    for y in range(32):
        m=v=0
        for x in range(32):
            r,g,b,a=px[x,y]
            if a >= 128:
                m |= 1 << (31-x); bounds=[min(bounds[0],x),min(bounds[1],y),max(bounds[2],x),max(bounds[3],y)]
                if (r+g+b)//3 >= 128: v |= 1 << (31-x)
        mask.append(m); image.append(v)
    if bounds[2] < 0: raise ValueError("cursor image is fully transparent")
    return point, bounds[2]+1, bounds[3]+1, mask, image

def emit_header(theme: str, out: pathlib.Path):
    rows=[]
    for name in DESKTOP:
        im,pt=raster(theme,name); rows.append((name,pack(im,pt)))
    license_id="CC-BY-SA-3.0" if theme.startswith("dmz") else "MIT"
    lines=[f"/* SPDX-License-Identifier: {license_id} */", f"/* Generated from {theme}; source attribution and licence are in assets/cursors/{theme}/. */", "#ifndef CIUKI_CURSOR_RASTERS_H", "#define CIUKI_CURSOR_RASTERS_H", "#include <stdint.h>", "struct desk_cursor_raster { unsigned hot_x, hot_y, width, height; uint32_t mask[32], image[32]; };", "static const struct desk_cursor_raster desk_cursors[] = {"]
    for name,(pt,w,h,mask,image) in rows:
        lines += [f"    /* {name} */", f"    {{{pt[0]}, {pt[1]}, {w}, {h}, {{"]
        lines += ["        "+", ".join(f"0x{x:08x}u" for x in mask[i:i+4])+"," for i in range(0,32,4)]
        lines += ["    }, {"]
        lines += ["        "+", ".join(f"0x{x:08x}u" for x in image[i:i+4])+"," for i in range(0,32,4)]
        lines += ["    }},"]
    if theme == "hackneyed":
        lines += ["};", "/* Animated busy artwork retained as sixteen ordered frames. */", "#define DESK_BUSY_FRAME_COUNT 16u", "#define DESK_BUSY_HOT_X 16u", "#define DESK_BUSY_HOT_Y 16u", "static const uint32_t desk_busy_frames[DESK_BUSY_FRAME_COUNT][32] = {"]
        frame_images=[]
        for frame in range(1,17):
            frame_im,frame_point=raster(theme,"busy",frame); _,_,_,frame_mask,frame_image=pack(frame_im,frame_point)
            lines.append("    {"+", ".join(f"0x{x:08x}u" for x in frame_mask)+"},")
            frame_images.append(frame_image)
        lines += ["};", "static const uint32_t desk_busy_frame_images[DESK_BUSY_FRAME_COUNT][32] = {"]
        lines += ["    {"+", ".join(f"0x{x:08x}u" for x in frame)+"}," for frame in frame_images]
    else:
        lines += ["};"]
    lines += ["#endif", ""]
    out.parent.mkdir(parents=True,exist_ok=True); out.write_text("\n".join(lines),encoding="utf-8")

def sheet(theme: str, out: pathlib.Path):
    names=DESKTOP+PAINT; cellw,cellh=150,112; cols=5; rows=(len(names)+cols-1)//cols
    im=Image.new("RGB",(cols*cellw,rows*cellh),(232,232,232)); draw=ImageDraw.Draw(im)
    for i,name in enumerate(names):
        cur,pt=raster(theme,name); x=(i%cols)*cellw; y=(i//cols)*cellh
        tile=Image.new("RGBA",(32,32),(255,255,255,255)); tile.alpha_composite(cur,(0,0)); tile=tile.resize((64,64),Image.Resampling.NEAREST)
        im.paste(tile.convert("RGB"),(x+42,y+5)); draw.ellipse((x+42+pt[0]*2-3,y+5+pt[1]*2-3,x+42+pt[0]*2+3,y+5+pt[1]*2+3),fill=(255,0,0))
        mapped=(DMZ_MAP if theme.startswith("dmz") else HACK_SVG[HACK_MAP[name]])
        draw.text((x+4,y+77),f"{name} -> {mapped}",fill=(0,0,0))
        draw.text((x+4,y+94),f"hot spot {pt[0]},{pt[1]}",fill=(80,20,20))
    out.parent.mkdir(parents=True,exist_ok=True); im.save(out,optimize=False)

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument("theme",choices=["dmz-white","dmz-black","hackneyed"]); p.add_argument("--output",type=pathlib.Path,default=ROOT/"assets/cursors/cursors.h"); p.add_argument("--sheet",type=pathlib.Path); a=p.parse_args()
    emit_header(a.theme,a.output)
    if a.sheet: sheet(a.theme,a.sheet)
    dest=ROOT/"assets/cursors"/a.theme; dest.mkdir(parents=True,exist_ok=True)
    source=(DMZ/"debian/copyright") if a.theme.startswith("dmz") else (HACK/"LICENSE")
    shutil.copyfile(source,dest/"LICENSE")
    credit=("DMZ Cursor Theme 0.4.5, Copyright 2007–2010 Novell, Inc. Licensed CC BY-SA 3.0. Adapted rasters and this adaptation are distributed under CC BY-SA 3.0; attribution and share-alike apply." if a.theme.startswith("dmz") else "Hackneyed X11 cursor theme, Copyright Richard Ferreira. MIT license; retain the copyright and permission notice in copies or substantial portions.")
    (dest/"ATTRIBUTION.txt").write_text(credit+"\n",encoding="utf-8")
if __name__=="__main__": main()
