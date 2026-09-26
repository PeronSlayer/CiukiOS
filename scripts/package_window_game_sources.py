#!/usr/bin/env python3
"""Include corresponding sources with the GPL windowed game binaries."""
import argparse,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.parent.mkdir(parents=True,exist_ok=True)
    paths=['third_party/doomgeneric','third_party/wolf4sdl','src/ports',
           'src/com/graphics_bridge_abi.inc','src/com/window_game_launch.asm',
           'scripts/build_doom_window.py','scripts/build_wolf_window.py']
    with tarfile.open(args.output,'w:gz') as archive:
        for name in paths:
            archive.add(ROOT/name,arcname=name,filter=lambda info:None if '__pycache__' in info.name else info)
if __name__=='__main__':main()
