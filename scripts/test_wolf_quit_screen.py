#!/usr/bin/env python3
"""Validate exact quit-message recognition against captured original Wolf."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from wolf_screen import WolfPictures, WolfQuitMessages, quit_confirmation_score

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/wolf-quit-screen-20261008')
    args = parser.parse_args()
    output = args.output.resolve()
    assert ROOT / 'build' in output.parents
    output.mkdir(parents=True, exist_ok=True)
    captured = ROOT / 'build/full/t23-next/qemu-wolf-files-ready-20261008'
    runtime = json.loads((captured / 'report.json').read_text())
    fat = FAT16(captured / 'disk.img')
    pictures = WolfPictures(*(fat.read('APPS/WOLF3D/' + name + '.WL6')
                              for name in ('VGAHEAD', 'VGADICT', 'VGAGRAPH')))
    messages = WolfQuitMessages(fat.read('APPS/WOLF3D/WOLF3D.EXE'))
    checks = []
    report = dict(passed=False, checks=checks, quit_message_asset=messages.evidence)

    def read(name):
        with Image.open(captured / (name + '-client.png')) as image:
            return np.array(image.convert('RGB'))

    def check(name, frame, wanted, message=None):
        result = quit_confirmation_score(frame, pictures, messages)
        checks.append(dict(name=name, expected_confirmation=wanted, **result))
        assert result['matched'] == wanted, checks[-1]
        if message is not None:
            assert result['message'] == message, checks[-1]

    try:
        with Image.open(captured / 'failure.png') as image:
            dialog = np.array(image.convert('RGB').crop(runtime['client_rectangle'])
                              .resize((320, 200), Image.Resampling.NEAREST))
        check('captured-real-three-line-confirmation', dialog, True, 4)
        for name in ('wolf-quit-confirmation', 'wolf-after-right', 'wolf-main-menu',
                     'wolf-episode-menu', 'wolf-normal-difficulty'):
            check('captured-' + name, read(name), False)
        check('blank-client', np.zeros_like(dialog), False)
        check('unrelated-colourful-client', np.random.default_rng(81).integers(
            0, 256, dialog.shape, dtype=np.uint8), False)
        for number, message in enumerate(messages.messages):
            # Matcher-only positives retain installed text glyphs. They are
            # independent of QEMU and do not claim additional runtime dialogs.
            frame = read('wolf-after-right')
            lines = [pictures.text(line, 1) for line in message.split('\n')]
            height = sum(line.shape[0] for line in lines)
            width = max([line.shape[1] for line in lines[:-1]] + [lines[-1].shape[1] + 10])
            x, y = 160 - width // 2, 80 - height // 2
            frame[y-5:y+height+5, x-5:x+width+5] = 142
            for line in lines:
                frame[y:y+line.shape[0], x:x+line.shape[1]] = np.where(line[..., None], 0, 142)
                y += line.shape[0]
            check(f'installed-message-{number}-synthetic-font-layout', frame, True, number)
        missing = dialog.copy()
        missing[87:100, 40:191] = 142
        check('captured-dialog-missing-final-line', missing, False)
        wrong = dialog.copy()
        wrong[61:74, 40:280] = 142
        check('captured-dialog-missing-first-line', wrong, False)
        check('captured-dialog-unready-dark-palette', (dialog * .5).astype(np.uint8), False)
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Wolf quit: {len(checks)} captured/asset cases PASS; {output / "report.json"}')


if __name__ == '__main__':
    main()
