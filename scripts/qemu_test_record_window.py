#!/usr/bin/env python3
"""Check a fixed QEMU GTK window across desktop, DOS and VGA preview modes."""
import json
import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OUT = ROOT / 'build/tests/qemu-record-web-20261001/record-window'
OUT.mkdir(parents=True, exist_ok=True)
DISK = OUT / 'disk.img'
SERIAL = OUT / 'serial.log'
MONITOR = OUT / 'monitor.sock'
MONITOR.unlink(missing_ok=True)
shutil.copyfile(ROOT / 'build/full/ciukios-full.img', DISK)
env = os.environ.copy()
env.update({
    'CIUKIOS_FULL_IMG': str(DISK),
    'QEMU_VISUAL_LOG': str(SERIAL),
    'QEMU_RECORD_MONITOR_SOCKET': str(MONITOR),
    'QEMU_AUDIO_MODE': 'off',
    'QEMU_NET_HOST_FTP_PORT': '0',
})
log = (OUT / 'runner.log').open('w')
proc = subprocess.Popen(['bash', 'scripts/qemu_record_full.sh'], env=env,
                        stdout=log, stderr=subprocess.STDOUT)
result = {'window_size': [1280, 800], 'checks': {}}


def until(predicate, seconds=50):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError('QEMU exited early; see runner.log')
        value = predicate()
        if value:
            return value
        time.sleep(.2)
    raise AssertionError('timed out waiting for QEMU')


def hmp(command):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(MONITOR))
        data = b''
        while b'(qemu)' not in data:
            chunk = connection.recv(4096)
            if not chunk:
                raise RuntimeError('QEMU monitor closed before prompt')
            data += chunk
        connection.sendall((command + '\n').encode())
        if command == 'quit':
            return b''
        data = b''
        while b'(qemu)' not in data:
            chunk = connection.recv(4096)
            if not chunk:
                raise RuntimeError('QEMU monitor closed after command')
            data += chunk
        return data


def serial_has(marker):
    if not SERIAL.exists():
        return False
    normalized = subprocess.check_output(['scripts/serial_log_normalize.py', str(SERIAL)])
    return marker.encode() in normalized


def geometry(window_id):
    raw = subprocess.check_output(['xdotool', 'getwindowgeometry', '--shell', window_id],
                                  stderr=subprocess.DEVNULL, text=True)
    fields = dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)
    return [int(fields['WIDTH']), int(fields['HEIGHT'])]


def find_window(qemu_pid):
    try:
        return subprocess.check_output(
            ['xdotool', 'search', '--pid', qemu_pid, '--name', '^CiukiOS$'],
            stderr=subprocess.DEVNULL, text=True).split()[:1]
    except subprocess.CalledProcessError:
        return []


def check_window(name, window_id, guest_size):
    until(lambda: geometry(window_id) == [1280, 800], 6)
    screenshot = OUT / f'{name}.ppm'
    hmp(f'screendump {screenshot}')
    until(lambda: screenshot.exists() and screenshot.stat().st_size > 64, 5)
    actual_guest_size = list(Image.open(screenshot).size)
    assert actual_guest_size == guest_size, (name, actual_guest_size, guest_size)
    result['checks'][name] = {'window': geometry(window_id), 'guest': actual_guest_size}


try:
    until(MONITOR.exists, 20)
    until(lambda: serial_has('[DESKTOP] READY'), 90)
    children = subprocess.check_output(['pgrep', '-P', str(proc.pid)], text=True).split()
    qemu_children = [pid for pid in children if subprocess.check_output(
        ['ps', '-p', pid, '-o', 'comm='], text=True).strip().startswith('qemu-system')]
    assert len(qemu_children) == 1, children
    qemu_pid = qemu_children[0]
    window = until(lambda: find_window(qemu_pid), 10)[0]
    check_window('desktop', window, [1024, 768])
    hmp('sendkey f4 30')
    until(lambda: serial_has('CiukiOS SHELL C:\\APPS>'), 20)
    check_window('dos', window, [1024, 768])
    for char in 'vgasetup desktop 800':
        hmp('sendkey ' + ('spc' if char == ' ' else char) + ' 30')
    hmp('sendkey ret 30')
    until(lambda: serial_has('[VGASETUP] DESKTOP PREVIEW 800 x 600'), 20)
    check_window('vga-preview', window, [800, 600])
    result['passed'] = True
finally:
    if proc.poll() is None:
        try:
            hmp('quit')
        except (OSError, TimeoutError, RuntimeError):
            proc.terminate()
    proc.wait(timeout=10)
    log.close()
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')

print(f'[qemu-record-window] PASS {OUT}')
