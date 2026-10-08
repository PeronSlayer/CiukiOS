#!/usr/bin/env python3
"""Reject captured preload/fade HUDs and retain exact settled gameplay guards."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from wolf_screen import WolfPalette, WolfPictures, gameplay_score, hud_score

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/wolf-gameplay-screen-20261008')
    args = parser.parse_args()
    output = args.output.resolve()
    assert ROOT / 'build' in output.parents
    output.mkdir(parents=True, exist_ok=True)
    captured = ROOT / 'build/full/t23-next/qemu-wolf-files-final-20261008'
    fat = FAT16(captured / 'disk.img')
    pictures = WolfPictures(*(fat.read('APPS/WOLF3D/' + name + '.WL6')
                              for name in ('VGAHEAD', 'VGADICT', 'VGAGRAPH')))
    palette = WolfPalette(fat.read('APPS/WOLF3D/WOLF3D.EXE'))
    checks = []
    report = dict(passed=False, checks=checks)

    def check(name, frame, wanted):
        result = gameplay_score(frame, pictures, palette)
        legacy = hud_score(frame, pictures)
        checks.append(dict(name=name, expected_settled=wanted, legacy_hud_matched=legacy['matched'], **result))
        assert result['matched'] == wanted, checks[-1]
        return result

    def read(name):
        with Image.open(captured / (name + '-client.png')) as image:
            return np.array(image.convert('RGB'))

    try:
        for name in ('wolf-gameplay-hud', 'wolf-passive-0', 'wolf-passive-1', 'wolf-passive-2'):
            result = check('captured-preload-' + name, read(name), False)
            assert result['loading_picture']['matched']
            assert checks[-1]['legacy_hud_matched']
        moved = read('wolf-after-right')
        result = check('captured-active-level-during-dark-fade', moved, False)
        assert not result['loading_picture']['matched'] and result['full_palette_score'] < .995
        for name in ('wolf-main-menu', 'wolf-episode-menu', 'wolf-normal-difficulty'):
            check('captured-' + name, read(name), False)
        check('blank-client', np.zeros_like(moved), False)
        check('unrelated-colourful-client', np.random.default_rng(81).integers(
            0, 256, moved.shape, dtype=np.uint8), False)
        # Asset-based positive: retain the captured actual level viewport and
        # install the unmodified static HUD at its complete palette in memory.
        # This is matcher validation, not a claim of a completed runtime gate.
        settled = moved.copy()
        settled[160:] = palette.rgb[pictures.picture(86)]
        check('captured-level-with-synthetic-complete-asset-hud', settled, True)
        dynamic = settled.copy()
        for x, y, width, height in ((136, 4, 24, 32), (240, 4, 16, 32),
                                   (256, 8, 64, 32), (16, 16, 16, 16),
                                   (48, 16, 48, 16), (112, 16, 8, 16),
                                   (168, 16, 24, 16), (216, 16, 16, 16)):
            dynamic[160+y:160+y+height, x:x+width] = (255, 0, 255)
        check('documented-dynamic-hud-fields-remain-excluded', dynamic, True)
        damaged = settled.copy()
        damaged[160:164] = 0
        check('static-hud-border-corruption-is-rejected', damaged, False)
        for fraction in (.5, .9, .97):
            check(f'partial-hud-palette-{fraction}', (settled * fraction).astype(np.uint8), False)
        # A complete HUD remains insufficient during real preloading.
        preload = read('wolf-passive-2')
        check('complete-palette-preload-with-valid-hud-is-not-gameplay', preload, False)
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wolf gameplay: {len(checks)} captured/asset cases PASS; {output / "report.json"}')


if __name__ == '__main__':
    main()
