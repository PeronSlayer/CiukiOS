#!/usr/bin/env python3
"""Lead-only NetSurf UI/internet probe. Run under the required 1500M/0 scope.

Uses the canonical image, user networking and a disposable qcow2 overlay.
No rebuilding. --dry-run validates the URL and prints the input plan without
starting QEMU. Coordinates can be adjusted for a different desktop layout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
from urllib.parse import urlsplit

import qmp
import resources
from control_display_probe import key, ppm_to_png

ROOT = Path(__file__).resolve().parents[2]


def url_keys(url):
    if any(ord(char) < 33 or ord(char) > 126 for char in url):
        raise ValueError('URL must contain printable ASCII without whitespace')
    parsed = urlsplit(url)
    if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('URL must be HTTP or HTTPS without credentials')
    if len(url) > 4096:
        raise ValueError('URL exceeds 4096 characters')
    punctuation = {'.': ('dot',), '/': ('slash',), ':': ('shift', 'semicolon'),
                   '-': ('minus',), '_': ('shift', 'minus'), '?': ('shift', 'slash'),
                   '=': ('equal',), '&': ('shift', '7'), '%': ('shift', '5'),
                   '#': ('shift', '3'), '+': ('shift', 'equal'), '@': ('shift', '2'),
                   '~': ('shift', 'grave_accent'), ';': ('semicolon',)}
    result = []
    for char in url:
        if 'a' <= char <= 'z' or '0' <= char <= '9':
            result.append((char,))
        elif 'A' <= char <= 'Z':
            result.append(('shift', char.lower()))
        elif char in punctuation:
            result.append(punctuation[char])
        else:
            raise ValueError('URL contains a character unsupported by the QMP typing probe')
    return result


def tap(client, keys):
    client.input_events([key(k, True) for k in keys])
    time.sleep(.03)
    client.input_events([key(k, False) for k in reversed(keys)])


def click(client, x, y):
    # Establish a known PS/2 origin, then move to desktop coordinates.
    for dx, dy in ((-4096, -4096), (x, y)):
        client.input_events([{'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
                             {'type': 'rel', 'data': {'axis': 'y', 'value': dy}}])
        time.sleep(.15)
    client.input_events([{'type': 'btn', 'data': {'down': True, 'button': 'left'}}])
    time.sleep(.06)
    client.input_events([{'type': 'btn', 'data': {'down': False, 'button': 'left'}}])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--url', default='http://example.com')
    ap.add_argument('--image', type=Path, default=ROOT / 'build/f0/ciukios.img')
    ap.add_argument('--run', type=Path, default=ROOT / 'build/test-runs/netsurf-probe')
    ap.add_argument('--nic', choices=('e1000', 'i82559er'), default='i82559er')
    ap.add_argument('--seconds', type=int, default=20)
    ap.add_argument('--desktop-wait', type=float, default=12)
    ap.add_argument('--browser-wait', type=float, default=15)
    ap.add_argument('--boot-timeout', type=float, default=240)
    ap.add_argument('--menu-x', type=int, default=215)
    ap.add_argument('--menu-y', type=int, default=90)
    ap.add_argument('--bar-x', type=int, default=430)
    ap.add_argument('--bar-y', type=int, default=112)
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args(argv)
    try:
        typing = url_keys(args.url)
    except ValueError as error:
        ap.error(str(error))
    if not 1 <= args.seconds <= 60:
        ap.error('--seconds must be between 1 and 60')
    if args.dry_run:
        print(json.dumps({'nic': args.nic, 'url': args.url, 'route': ['meta_l', 'right', 'menu-click',
                         'address-click', 'ctrl-a', 'type-url', 'ret'], 'typing': typing,
                         'screenshots': args.seconds}, indent=2))
        return
    run = args.run.resolve()
    if not run.is_relative_to(ROOT / 'build/test-runs'):
        ap.error('--run must be inside this worktree build/test-runs')
    image = args.image.resolve()
    with image.open('rb') as stream:
        image_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    with resources.ExclusiveLock(resources.common_lock(ROOT)):
        resources.preflight(ROOT / 'build/test-runs', wait_seconds=0)
        run.mkdir(parents=True, exist_ok=False)
        overlay = run / 'disk.qcow2'
        subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-b', str(image),
                        '-F', 'raw', str(overlay)], check=True)
        profile = json.loads((ROOT / 'tests/profiles/qemu-t23.json').read_text())
        firmware = next(p for p in (Path('/usr/share/qemu/bios-256k.bin'),
                                   Path('/usr/share/seabios/bios-256k.bin')) if p.exists())
        serial = run / 'serial.log'
        sock = run / 'q'
        command = ['qemu-system-i386', '-machine', profile['machine'], '-cpu', profile['cpu'],
                   '-accel', 'tcg', '-m', str(profile['ram_mib']), '-smp', '1', '-bios', str(firmware),
                   '-display', 'none', '-monitor', 'none', '-no-shutdown', '-no-reboot', '-S',
                   '-drive', f'file={overlay},format=qcow2,if=ide,index=0,media=disk,cache=writeback',
                   '-vga', 'std', '-serial', f'file:{serial}', '-audiodev', 'none,id=a0',
                   '-device', 'AC97,audiodev=a0', '-netdev', 'user,id=net0',
                   '-device', f'{args.nic},netdev=net0', '-rtc', 'base=utc,clock=host',
                   '-qmp', f'unix:{sock},server=on,wait=off']
        with (run / 'qemu.log').open('w') as log, (run / 'qmp.log').open('wb') as qlog:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=run)
            client = None
            try:
                deadline = time.monotonic() + args.boot_timeout
                while not sock.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('QMP socket did not become ready')
                    time.sleep(.1)
                client = qmp.QMP(sock, qlog)
                client.command('cont')
                while b'spawning /Programs/desktop' not in (serial.read_bytes() if serial.exists() else b''):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('desktop did not start')
                    time.sleep(.25)
                time.sleep(args.desktop_wait)
                tap(client, ('meta_l',));time.sleep(.5)
                tap(client, ('right',));time.sleep(1)
                click(client, args.menu_x, args.menu_y)
                time.sleep(args.browser_wait)
                click(client, args.bar_x, args.bar_y)
                tap(client, ('ctrl', 'a'))
                for keys in typing:
                    tap(client, keys)
                tap(client, ('ret',))
                opened = time.monotonic()
                for second in range(1, args.seconds + 1):
                    time.sleep(max(0, opened + second - time.monotonic()))
                    capture = run / f'{second:02d}.ppm'
                    client.command('screendump', {'filename': str(capture)})
                    ppm_to_png(capture, capture.with_suffix('.png'))
                client.command('quit')
            finally:
                if client:
                    client.socket.close()
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait()
        with image.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != image_hash:
                raise RuntimeError('canonical image changed during probe')
        overlay.unlink()
        lines = [line for line in serial.read_text(errors='replace').splitlines()
                 if '[netsurf]' in line or '[net] tcp ' in line]
        (run / 'summary.txt').write_text('image_sha256=' + image_hash + '\n' + '\n'.join(lines) + '\n')
        print('Evidence:', run)


if __name__ == '__main__':
    main()
