#!/usr/bin/env python3
"""Keep the naturally dark installed Episode 1 thumbnail and cursor strict."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from wolf_screen import WolfPictures, episode_score, pattern_score

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-wolf-files-signon-20261008/disk.img')
    parser.add_argument('--captured', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-wolf-files-signon-20261008/wolf-episode-menu-client.png')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/wolf-episode-screen-20261008')
    args = parser.parse_args()
    output = args.output.resolve()
    assert ROOT / 'build' in output.parents
    output.mkdir(parents=True, exist_ok=True)
    fat = FAT16(args.image.resolve())
    assets = [fat.read('APPS/WOLF3D/' + name + '.WL6')
              for name in ('VGAHEAD', 'VGADICT', 'VGAGRAPH')]
    pictures = WolfPictures(*assets)
    with Image.open(args.captured) as image:
        captured = np.array(image.convert('RGB'))
    checks = []
    report = dict(passed=False, screenshot_sha256=hashlib.sha256(args.captured.read_bytes()).hexdigest(),
                  asset_sha256=[hashlib.sha256(asset).hexdigest() for asset in assets], checks=checks)

    def check(name, frame, wanted):
        result = episode_score(frame, pictures)
        checks.append(dict(name=name, expected_episode=wanted, **result))
        assert result['matched'] == wanted, checks[-1]
        return result

    try:
        result = check('captured-complete-episode1-menu', captured, True)
        assert result['score'] == 1.0 and result['pixels'] == 955
        assert result['distinct_colours'] == 10 and result['contrast'] == 65
        assert result['cursor']['score'] == 1.0
        default = pattern_score(captured, pictures.picture(30), 40, 23)
        assert not default['matched'] and default['contrast'] == 65
        checks.append(dict(name='default80-guard-is-unchanged', **default))
        check('blank-client', np.zeros_like(captured), False)
        check('colourful-unrelated-client', np.random.default_rng(93).integers(
            0, 256, captured.shape, dtype=np.uint8), False)
        for fraction in (.5, .9, .97):
            check(f'incomplete-palette-fade-{fraction}', (captured * fraction).astype(np.uint8), False)
        palette = np.random.default_rng(47).integers(0, 256, (256, 3), dtype=np.uint8)
        check('installed-title-image', palette[pictures.picture(87)], False)
        changed = captured.copy()
        h, w = pictures.picture(11).shape
        changed[21:21+h, 8:8+w] = 0
        check('exact-thumbnail-without-selected-cursor', changed, False)
        changed = captured.copy()
        region = changed[23:47, 40:88]
        region[:] = np.roll(region, 1, axis=1)
        check('shifted-thumbnail-keeps-cursor-but-fails-class-pixels', changed, False)
        changed = captured.copy()
        changed[23:35, 40:64] = (255, 255, 255)
        check('corrupt-thumbnail-keeps-cursor-but-fails-class-pixels', changed, False)
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wolf episode: {len(checks)} captured/negative cases PASS; {output / "report.json"}')


if __name__ == '__main__':
    main()
