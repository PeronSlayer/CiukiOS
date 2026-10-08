#!/usr/bin/env python3
"""Keep fading Wolf assets unready until the installed full palette matches."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from wolf_screen import WolfPalette, WolfPictures, complete_picture_score, pattern_score

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/wolf-intro-screen-20261008')
    args = parser.parse_args()
    output = args.output.resolve()
    assert ROOT / 'build' in output.parents
    output.mkdir(parents=True, exist_ok=True)
    previous = ROOT / 'build/full/t23-next/qemu-wolf-files-signon-20261008'
    latest = ROOT / 'build/full/t23-next/qemu-wolf-files-gameplay-20261008'
    fat = FAT16(latest / 'disk.img')
    palette = WolfPalette(fat.read('APPS/WOLF3D/WOLF3D.EXE'))
    pictures = WolfPictures(*(fat.read('APPS/WOLF3D/' + name + '.WL6')
                              for name in ('VGAHEAD', 'VGADICT', 'VGAGRAPH')))
    checks = []
    report = dict(passed=False, palette_asset=palette.evidence, checks=checks)

    def check(name, frame, number, x, y, wanted):
        result = complete_picture_score(frame, pictures.picture(number), x, y, palette)
        legacy = pattern_score(frame, pictures.picture(number), x, y)
        checks.append(dict(name=name, expected_complete=wanted, legacy_matched=legacy['matched'], **result))
        assert result['matched'] == wanted, checks[-1]
        return legacy

    try:
        with Image.open(latest / 'wolf-main-menu-client.png') as image:
            check('captured-dark-title-fade', np.array(image.convert('RGB')), 87, 0, 0, False)
        with Image.open(previous / 'wolf-main-menu-client.png') as image:
            check('captured-full-main-menu-options', np.array(image.convert('RGB')), 10, 80, 0, True)
        title = palette.rgb[pictures.picture(87)]
        check('installed-full-title', title, 87, 0, 0, True)
        for fraction in (.3, .5, .9, .97):
            legacy = check(f'incomplete-title-fade-{fraction}', (title * fraction).astype(np.uint8),
                           87, 0, 0, False)
            if fraction >= .5:
                assert legacy['matched'], 'Regression must show class-only recognition is premature'
        check('blank-client', np.zeros_like(title), 87, 0, 0, False)
        check('colourful-unrelated-client', np.random.default_rng(43).integers(
            0, 256, title.shape, dtype=np.uint8), 87, 0, 0, False)
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wolf intro: {len(checks)} captured/palette cases PASS; {output / "report.json"}')


if __name__ == '__main__':
    main()
