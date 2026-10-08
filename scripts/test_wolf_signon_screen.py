#!/usr/bin/env python3
"""Replay the real Wolf sign-on pause and reject unrelated/incomplete screens."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from wolf_screen import WolfPictures, WolfSignon, signon_score

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-wolf-files-edges-20261008/disk.img')
    parser.add_argument('--captured', type=Path,
                        default=ROOT / 'build/full/t23-next/qemu-wolf-files-edges-20261008/wolf-intro-client.png')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/wolf-signon-screen-20261008')
    args = parser.parse_args()
    output = args.output.resolve()
    assert ROOT / 'build' in output.parents
    output.mkdir(parents=True, exist_ok=True)
    fat = FAT16(args.image.resolve())
    engine = fat.read('APPS/WOLF3D/WOLF3D.EXE')
    signon = WolfSignon(engine)
    pictures = WolfPictures(*(fat.read('APPS/WOLF3D/' + name + '.WL6')
                              for name in ('VGAHEAD', 'VGADICT', 'VGAGRAPH')))
    with Image.open(args.captured) as image:
        captured = np.array(image.convert('RGB'))
    assert captured.shape == (200, 320, 3)
    checks = []
    report = dict(passed=False, engine_sha256=hashlib.sha256(engine).hexdigest(),
                  signon_asset=signon.evidence,
                  screenshot_sha256=hashlib.sha256(args.captured.read_bytes()).hexdigest(),
                  checks=checks)

    def check(name, frame, wanted):
        result = signon_score(frame, pictures, signon)
        item = dict(name=name, expected_waiting=wanted, **result)
        checks.append(item)
        assert result['matched'] == wanted, item

    try:
        check('captured-available-memory-awaiting-key', captured, True)
        changed = captured.copy()
        for x in (49, 89, 129):
            for row in range(10):
                y = 163 - row * 8
                changed[y:y+5, x:x+6] = (27, 181, 9)
        for y in (82, 105, 128, 151, 174):
            changed[y:y+2, 164:176] = (255, 0, 255)
        check('different-documented-hardware-and-memory-indicators', changed, True)
        check('same-indexed-pattern-different-palette', 255 - captured, True)
        check('blank-client', np.zeros_like(captured), False)
        check('unrelated-colourful-client', np.random.default_rng(19).integers(
            0, 256, captured.shape, dtype=np.uint8), False)
        changed = captured.copy()
        changed[189:200, :300] = (138, 0, 0)
        check('signon-with-no-waiting-prompt', changed, False)
        working = pictures.text('Working...')
        x = (320 - working.shape[1]) // 2
        changed[190:190+working.shape[0], x:x+working.shape[1]][working] = (255, 255, 85)
        check('working-transition-is-not-ready-for-acknowledgement', changed, False)
        changed = captured.copy()
        changed[190:200] = np.roll(changed[190:200], 1, axis=1)
        check('shifted-prompt-is-not-exact-installed-font', changed, False)
        changed = captured.copy()
        changed[:65] = 0
        check('prompt-and-body-without-signon-header', changed, False)
        changed = np.zeros_like(captured)
        changed[189:] = captured[189:]
        check('waiting-prompt-alone-is-not-signon', changed, False)
        palette = np.random.default_rng(47).integers(0, 256, (256, 3), dtype=np.uint8)
        title = palette[pictures.picture(87)]
        check('actual-installed-title-pattern', title, False)
        title[189:] = captured[189:]
        check('title-with-waiting-prompt-is-not-signon', title, False)

        damaged = bytearray(engine)
        damaged[signon.evidence['file_offset'] + 4000] ^= 1
        try:
            WolfSignon(damaged)
        except AssertionError as exc:
            checks.append(dict(name='changed-linked-bitmap-is-rejected', rejected=str(exc)))
        else:
            raise AssertionError('Changed signon asset silently accepted')
        try:
            WolfSignon(b'COM' + engine[3:])
        except AssertionError as exc:
            checks.append(dict(name='non-MZ-engine-is-rejected', rejected=str(exc)))
        else:
            raise AssertionError('Non-MZ engine silently accepted')
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wolf signon: {len(checks)} captured/negative cases PASS; {output / "report.json"}')


if __name__ == '__main__':
    main()
