#!/usr/bin/env python3
"""Lead-only f6-23 probe: Programs -> NetSurf -> address -> example.com.

Use the canonical image under the normal capped QEMU scope, one guest at a
time. No build. A qcow2 overlay protects the image. The default coordinates
describe the 1024x768 desktop, without installed Programs-folder shortcuts;
override --entry-x/--entry-y for other menu contents. --layout must match the
desktop keyboard setting. Records the image hash, serial/QMP logs and one
PNG per second for 20 seconds after Enter. This is diagnostic evidence, not
an automatic assertion of successful network navigation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

import qmp
import resources
from control_display_probe import key, ppm_to_png

ROOT = Path(__file__).resolve().parents[2]


def tap(q, codes):
    q.input_events([key(code, True) for code in codes])
    time.sleep(.06)
    q.input_events([key(code, False) for code in reversed(codes)])
    time.sleep(.06)


def click(q, x, y):
    # Home to the clipped corner: no assumption about the initial cursor.
    q.input_events([{'type': 'rel', 'data': {'axis': axis, 'value': -32767}}
                    for axis in ('x', 'y')])
    time.sleep(.2)
    q.input_events([{'type': 'rel', 'data': {'axis': 'x', 'value': x}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': y}}])
    time.sleep(.3)
    for down in (True, False):
        q.input_events([{'type': 'btn', 'data': {'button': 'left', 'down': down}}])
        time.sleep(.06)


def type_url(q, layout):
    # Physical qcodes produce desktop TEXT in the selected keyboard layout.
    punctuation = {':': ['shift', 'semicolon'], '/': ['slash'], '.': ['dot']}
    if layout == 'it':
        punctuation.update({':': ['shift', 'dot'], '/': ['shift', '7']})
    for char in 'http://example.com':
        tap(q, punctuation.get(char, [char]))


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def run_probe(args):
    image = Path(args.image).resolve()
    run = Path(args.run).resolve()
    # Refuse overwrite rather than deleting another run's evidence.
    run.mkdir(parents=True, exist_ok=False)
    image_hash = digest(image)
    (run / 'image.sha256').write_text(f'{image_hash}  {image}\n')
    profile = json.loads((ROOT / 'tests/profiles/qemu-t23.json').read_text())
    overlay = run / 'disk.qcow2'
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-b', str(image),
                    '-F', 'raw', str(overlay)], check=True)
    bios = next(path for path in (Path('/usr/share/qemu/bios-256k.bin'),
                Path('/usr/share/seabios/bios-256k.bin')) if path.exists())
    socket = run / 'q'
    serial = run / 'serial.log'
    commands = ['qemu-system-i386', '-machine', profile['machine'], '-cpu', profile['cpu'],
        '-accel', 'tcg', '-m', str(profile['ram_mib']), '-smp', '1', '-bios', str(bios),
        '-display', 'none', '-monitor', 'none', '-no-shutdown', '-no-reboot', '-S',
        '-drive', f'file={overlay},format=qcow2,if=ide,index=0,media=disk,cache=writeback',
        '-vga', 'std', '-serial', f'file:{serial}', '-audiodev', 'none,id=a0',
        '-device', 'AC97,audiodev=a0', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
        '-qmp', f'unix:{socket},server=on,wait=off']
    with (run / 'qemu.log').open('w') as log, (run / 'qmp.log').open('wb') as qlog:
        process = subprocess.Popen(commands, stdout=log, stderr=subprocess.STDOUT, cwd=run)
        q = None
        try:
            deadline = time.monotonic() + 10
            while not socket.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('QMP socket did not appear; see qemu.log')
                time.sleep(.1)
            q = qmp.QMP(socket, qlog)
            q.command('cont')
            deadline = time.monotonic() + args.boot_timeout
            while b'spawning /Programs/desktop' not in (serial.read_bytes() if serial.exists() else b''):
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('desktop did not spawn')
                time.sleep(.25)
            time.sleep(args.desktop_wait)

            def shot(name):
                ppm = run / (name + '.ppm')
                q.command('screendump', {'filename': str(ppm)})
                ppm_to_png(ppm, ppm.with_suffix('.png'))

            shot('00-desktop')
            tap(q, ['meta_l'])
            time.sleep(.6)
            tap(q, ['right'])
            time.sleep(.8)
            shot('01-programs')
            click(q, args.entry_x, args.entry_y)
            time.sleep(args.browser_wait)
            shot('02-browser')
            click(q, args.address_x, args.address_y)
            type_url(q, args.layout)
            shot('03-address')
            tap(q, ['ret'])
            start = time.monotonic()
            for second in range(1, 21):
                time.sleep(max(0, start + second - time.monotonic()))
                shot(f'{second + 3:02d}-t{second:02d}s')
            q.command('quit')
        finally:
            if q is not None:
                q.socket.close()
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
    if digest(image) != image_hash:
        raise RuntimeError('canonical image changed during the probe')
    lines = serial.read_text(errors='replace').splitlines()
    relevant = [line for line in lines if '[netsurf]' in line or '[net]' in line]
    (run / 'summary.txt').write_text(f'image_sha256={image_hash}\nlayout={args.layout}\n' +
                                    '\n'.join(relevant) + '\n')
    overlay.unlink()
    print('Probe evidence:', run)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default=str(ROOT / 'build/f0/ciukios.img'))
    parser.add_argument('--run', default=str(ROOT / 'build/test-runs/netsurf-probe'))
    parser.add_argument('--layout', choices=('us', 'it'), default='us')
    parser.add_argument('--entry-x', type=int, default=260)
    parser.add_argument('--entry-y', type=int, default=90)
    parser.add_argument('--address-x', type=int, default=260)
    parser.add_argument('--address-y', type=int, default=80)
    parser.add_argument('--desktop-wait', type=float, default=12)
    parser.add_argument('--browser-wait', type=float, default=8)
    parser.add_argument('--boot-timeout', type=float, default=240)
    args = parser.parse_args()
    with resources.SlotLock(resources.common_lock(ROOT)):
        if resources.other_slots_held():
            raise resources.Refusal('another runner slot is held; run this probe alone')
        resources.preflight(ROOT)
        run_probe(args)


if __name__ == '__main__':
    main()
