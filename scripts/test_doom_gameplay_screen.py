#!/usr/bin/env python3
"""Replay captured false-readiness menus/wipes against real WAD/HUD pixels."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image

from doom_screen import gameplay_screen, menu_skulls, statusbar_patterns
from qemu_test_installed_hdd import FAT16

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT / 'build/full/ciukios-full.img')
    parser.add_argument('--negative-dir', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-doom-files-checked-20261008')
    parser.add_argument('--positive-dir', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-games-512-audio-ready-20261008')
    parser.add_argument('--transition-dir', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-games-wrapper-baseline-20261008')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/doom-gameplay-screen-20261008')
    args = parser.parse_args()
    output = args.output.resolve()
    assert ROOT / 'build' in output.parents
    output.mkdir(parents=True, exist_ok=True)
    wad = FAT16(args.image.resolve()).read('APPS/DOOM/DOOM.WAD')
    patterns, sprites = statusbar_patterns(wad), menu_skulls(wad)
    checks = []
    report = dict(passed=False, wad_sha256=hashlib.sha256(wad).hexdigest(), checks=checks)

    def check(name, image, wanted, source=None):
        result = gameplay_screen(image, patterns, sprites)
        pixels = np.asarray(image.resize((640, 400), Image.Resampling.NEAREST)).astype(np.int16)[336:]
        red = int(((pixels[:, :, 0] > 95) & (pixels[:, :, 0] > pixels[:, :, 1] + 40)
                   & (pixels[:, :, 0] > pixels[:, :, 2] + 40)).sum())
        nonblack = int(np.any(pixels > 20, axis=2).sum())
        item = dict(name=name, expected_gameplay=wanted, **result,
                    legacy_colour_check=red > 800 and nonblack > 20000)
        if source is not None:
            item.update(source=str(source.relative_to(ROOT)),
                        screenshot_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
        checks.append(item)
        assert result['matched'] == wanted, item
        return result

    def captured(directory, name, wanted):
        path = directory.resolve() / name
        with Image.open(path) as image:
            return check(name, image.convert('RGB'), wanted, path)

    try:
        for name in ('doom-first-running-client.png', 'doom-first-skill-menu-client.png',
                     'doom-first-title-client.png', 'doom-first-title-menu-client.png',
                     'doom-first-episode-menu-client.png'):
            result = captured(args.negative_dir, name, False)
            if name in ('doom-first-running-client.png', 'doom-first-skill-menu-client.png'):
                assert result['menus']['skill'] == 2, result
        for name in ('doom-first-after-input-client.png', 'doom-second-after-input-client.png'):
            captured(args.positive_dir, name, True)
        captured(args.transition_dir, 'doom-second-after-input-client.png', True)
        for name in ('doom-first-running-client.png', 'doom-first-after-input-client.png'):
            captured(args.transition_dir, name, False)
        check('blank-client', Image.new('RGB', (320, 200)), False)

        # A real HUD behind a menu must still fail, regardless of the menu's
        # background scene. Use actual WAD skull pixels, without saved assets.
        with Image.open(args.positive_dir / 'doom-second-after-input-client.png') as image:
            hud = np.array(image.convert('RGB').resize((320, 200), Image.Resampling.NEAREST))
        pixels, sprite_mask, left, top = sprites[0]
        h, w = sprite_mask.shape
        for name, origin, row in (('main', (97, 64), 0), ('episode', (48, 63), 0),
                                  ('skill', (48, 63), 2)):
            frame = hud.copy()
            x, y = origin[0]-32-left, origin[1]-5+row*16-top
            region = frame[y:y+h, x:x+w]
            region[sprite_mask] = pixels[sprite_mask]
            result = check('HUD-with-' + name + '-menu', Image.fromarray(frame), False)
            assert result['statusbar']['matched'], 'Negative must retain its valid HUD'
            assert result['menus'][name] == row, 'Menu sprite not recognized'

        # Changing only the masked dynamic status widgets must retain readiness.
        frame = hud.copy()
        dynamic = ~patterns[0][1]
        frame[168:][dynamic] = np.random.default_rng(32).integers(0, 256, (dynamic.sum(), 3), dtype=np.uint8)
        check('HUD-changing-widgets', Image.fromarray(frame), True)
        assert any(item['legacy_colour_check'] and not item['matched'] for item in checks), \
            'Captured regressions did not demonstrate the old false-positive criterion'
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Doom readiness: {len(checks)} captured/synthetic cases PASS; {output / "report.json"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
