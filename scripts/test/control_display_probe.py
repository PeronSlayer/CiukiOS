#!/usr/bin/env python3
"""f2-88b probe: open Display Properties on the qemu-t23 profile and record where the time goes.

Boots the canonical image (no icount, -S then QMP cont), waits for the desktop (serial
"spawning /Programs/desktop" plus 12 s), then drives the keyboard route
  Logo key -> Settings -> Control Panel list -> Appearance -> Display -> Enter
(shell.c menu_key: Down from no selection picks the first item, Right opens a submenu with its first
item selected, the separator is skipped), screendumps once per second for 12 s after Enter, and keeps the
serial. Applet records: "[control] display open ms=.. decode=.. render=.. first_frame=.. ping_answered=.."
and "[control] display turn_gap_ms=..". Run one QEMU at a time under the capped scope, e.g.
  systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0 -- python3 scripts/test/control_display_probe.py
Output: build/test-runs/control-display-probe/{serial.log,qemu.log,qmp.log,NN-*.png,summary.txt}."""
import argparse, json, re, shutil, struct, subprocess, sys, time, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import qmp  # noqa: E402

# (label, qcode list pressed together then released, pause after in seconds)
ROUTE = [
    ('start', ['meta_l'], 1.5),
    ('down1', ['down'], .4), ('settings', ['down'], .4),
    ('open-settings', ['right'], 3.0),
    ('display', 'mouse', 0),  # double-click the Display item (list at ~265,141 from the pointer's centre 512,395)
]


def key(q, down):
    return {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': q}}}


def ppm_to_png(ppm, png):
    data = ppm.read_bytes()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    w, h = int(m.group(1)), int(m.group(2)); raw = data[m.end():]
    rows = b''.join(b'\0' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))
    ppm.unlink()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default=str(ROOT / 'build/f0/ciukios.img'))
    ap.add_argument('--run', default=str(ROOT / 'build/test-runs/control-display-probe'))
    ap.add_argument('--desktop-wait', type=float, default=12.0)
    ap.add_argument('--seconds', type=int, default=12)
    ap.add_argument('--boot-timeout', type=float, default=240.0)
    args = ap.parse_args()
    run = Path(args.run); shutil.rmtree(run, ignore_errors=True); run.mkdir(parents=True)
    prof = json.load(open(ROOT / 'tests/profiles/qemu-t23.json'))
    img = Path(args.image); ovl = run / 'disk.qcow2'
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-b', str(img), '-F', 'raw', str(ovl)], check=True)
    fw = next(p for p in (Path('/usr/share/qemu/bios-256k.bin'), Path('/usr/share/seabios/bios-256k.bin')) if p.exists())
    sock = run / 'q'; serial = run / 'serial.log'
    qargs = ['qemu-system-i386', '-machine', prof['machine'], '-cpu', prof['cpu'], '-accel', 'tcg',
             '-m', str(prof['ram_mib']), '-smp', '1', '-bios', str(fw), '-display', 'none', '-monitor', 'none', '-nic', 'none',
             '-no-shutdown', '-no-reboot', '-S', '-drive', f'file={ovl},format=qcow2,if=ide,index=0,media=disk,cache=writeback',
             '-vga', 'std', '-serial', f'file:{serial}', '-audiodev', 'none,id=a0', '-device', 'AC97,audiodev=a0',
             '-qmp', f'unix:{sock},server=on,wait=off']
    log = open(run / 'qemu.log', 'w')
    p = subprocess.Popen(qargs, stdout=log, stderr=subprocess.STDOUT, cwd=run)
    try:
        for _ in range(100):
            if sock.exists(): break
            time.sleep(.1)
        q = qmp.QMP(sock, open(run / 'qmp.log', 'wb'))
        q.command('cont'); t0 = time.monotonic(); shots = []
        def shot(name):
            f = run / f'{len(shots):02d}-{name}.ppm'
            q.command('screendump', {'filename': str(f)}); shots.append(f)
        while time.monotonic() - t0 < args.boot_timeout:
            if b'spawning /Programs/desktop' in (serial.read_bytes() if serial.exists() else b''): break
            time.sleep(.25)
        else:
            raise SystemExit('desktop never spawned')
        spawned = time.monotonic() - t0
        time.sleep(args.desktop_wait); shot('desktop')
        for label, keys, pause in ROUTE:
            if keys == 'mouse':
                # Settings submenu is open: hover Appearance (255,135), then click Display (412,136).
                q.input_events([{'type': 'rel', 'data': {'axis': 'x', 'value': -257}}, {'type': 'rel', 'data': {'axis': 'y', 'value': -260}}])
                time.sleep(.8); shot('hover-appearance')
                q.input_events([{'type': 'rel', 'data': {'axis': 'x', 'value': 157}}, {'type': 'rel', 'data': {'axis': 'y', 'value': 1}}])
                time.sleep(.4)
                q.input_events([{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]); time.sleep(.06)
                q.input_events([{'type': 'btn', 'data': {'down': False, 'button': 'left'}}])
                time.sleep(pause); continue
            q.input_events([key(k, True) for k in keys]); time.sleep(.08); q.input_events([key(k, False) for k in keys])
            if label in ('start', 'open-appearance'): time.sleep(.5); shot(label)
            time.sleep(pause)
        opened = time.monotonic()
        for i in range(args.seconds):
            time.sleep(max(0, opened + i + 1 - time.monotonic())); shot(f't{i + 1:02d}s')
        q.command('quit')
    finally:
        try: p.wait(10)
        except Exception: p.kill()
    for f in sorted(run.glob('*.ppm')): ppm_to_png(f, f.with_suffix('.png'))
    text = serial.read_text(errors='replace') if serial.exists() else ''
    lines = [l for l in text.splitlines() if '[control]' in l or 'control#' in l and '[cpu] top=' in l or '[desktop] stall' in l]
    (run / 'summary.txt').write_text(f'desktop spawned after {spawned:.1f} s; Enter sent at +{opened - t0:.1f} s\n' + '\n'.join(lines[-80:]) + '\n')
    print((run / 'summary.txt').read_text()); print('run directory:', run)


if __name__ == '__main__':
    main()
