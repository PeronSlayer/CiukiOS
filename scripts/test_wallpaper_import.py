#!/usr/bin/env python3
"""Check wallpaper file validation and non-destructive pack generation."""
import json
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

from build_wallpapers import build, catalog_bytes, convert, discover, read_catalog


def main():
    checks = []
    with tempfile.TemporaryDirectory(prefix='ciukios-wallpaper-') as temp:
        root = Path(temp)
        inputs = root/'inputs'; inputs.mkdir()
        source = Image.new('RGB',(17,19))
        source.putdata([((x%4)*50,(y%4)*50,90) for y in range(19) for x in range(17)])
        source.save(inputs/'Pattern.PNG')
        source.save(inputs/'Other.BmP',format='BMP')
        found = discover(inputs)
        assert len(found) == 2
        for item in found:
            payload, dimensions = convert(item)
            assert dimensions == (17,19)
            pal = payload[16:784]
            pixels = b''.join(pal[i*3:i*3+3] for i in payload[784:])
            assert pixels == source.tobytes()
        checks.append('case-insensitive PNG/BMP conversion is pixel-exact')
        output = root/'output'
        build(output, found, personal=True)
        records = read_catalog((output/'WALLS.DAT').read_bytes())
        assert len(records) == 2
        checks.append('catalog roundtrip')
        before = {p.name:p.read_bytes() for p in output.iterdir()}
        oversized = root/'oversized.png'; Image.new('RGB',(257,1)).save(oversized)
        transparent = root/'transparent.png'; Image.new('RGBA',(3,3),(1,2,3,0)).save(transparent)
        colors = root/'colors.png'; im=Image.new('RGB',(256,2));im.putdata([(i%256,i//256,1) for i in range(512)]);im.save(colors)
        corrupt = root/'corrupt.bmp';corrupt.write_bytes(b'not an image')
        for bad in (oversized,transparent,colors,corrupt):
            try: build(output, found+[bad], personal=True)
            except (ValueError,OSError): pass
            else: raise AssertionError(f'Invalid input accepted: {bad}')
            assert {p.name:p.read_bytes() for p in output.iterdir()} == before
        checks.append('invalid input leaves existing catalog and every tile unchanged')
        large = [(f'WALL{i:02}.CWP',b'Tile'.ljust(31,b'\0')) for i in range(1,100)]
        assert len(read_catalog(catalog_bytes(large))) == 99
        assert len(catalog_bytes(large)) == 4608
        try: catalog_bytes(large+[('WALL100.CWP',b'Tile'.ljust(31,b'\0'))])
        except ValueError: pass
        else: raise AssertionError('100th tile accepted')
        checks.append('99-slot bound and sector-sized catalog')
        pack = root/'pack'
        command=['python3','scripts/add_wallpapers.py','--after','11','--output',str(pack),str(found[0])]
        subprocess.run(command,check=True,capture_output=True)
        assert (pack/'WALL12.CWP').exists() and not (pack/'WALLS.DAT').exists()
        existing=(pack/'WALL12.CWP').read_bytes()
        assert subprocess.run(command,capture_output=True).returncode != 0
        assert (pack/'WALL12.CWP').read_bytes() == existing
        checks.append('append-only pack refuses overwrite and never replaces catalog')
    print(json.dumps({'status':'pass','checks':checks},indent=2))


if __name__ == '__main__': main()
