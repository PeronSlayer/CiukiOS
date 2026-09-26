#!/usr/bin/env python3
"""Qualify real read-only CD boot sessions, with no attached hard disks.

One Pentium III / 128 MiB QEMU instance runs at a time. This establishes
emulated boot/session behavior, not physical CPU speed or GPU compatibility.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import time
import uuid

from PIL import Image, ImageChops
from serial_log_normalize import normalize


ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            result.update(block)
    return result.hexdigest()


class Session:
    def __init__(self, iso, output, case, accelerator):
        self.output, self.case = output, case
        output.mkdir()
        self.serial = output/'serial.log'
        self.audio = output/'startup.wav' if case == 'muted' else None
        self.monitor = Path(tempfile.gettempdir())/f'ciukios-session-{uuid.uuid4().hex}.sock'
        self.command = [
            'qemu-system-i386', '-accel', accelerator,
            '-machine', 'pc,vmport=off,i8042=on', '-cpu', 'pentium3', '-smp', '1',
            '-m', '128', '-vga', 'std', '-nic', 'none',
            '-drive', f'file={iso},format=raw,if=ide,index=2,media=cdrom,readonly=on',
            '-boot', 'd', '-display', 'none', '-serial', f'file:{self.serial}',
            '-monitor', f'unix:{self.monitor},server=on,wait=off',
            '-audiodev', f'wav,id=snd0,path={self.audio}' if self.audio else 'none,id=snd0',
            '-device', 'sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0',
            '-device', 'adlib,audiodev=snd0', '-no-reboot', '-no-shutdown',
        ]
        (output/'command.json').write_text(json.dumps(self.command, indent=2)+'\n')
        (output/'command.txt').write_text(shlex.join(self.command)+'\n')
        self.errors = (output/'qemu.stderr.log').open('wb')
        self.start = time.monotonic()
        self.process = subprocess.Popen(self.command, stdout=subprocess.DEVNULL, stderr=self.errors)
        self.report = dict(case=case, status='running', cpu='pentium3', ram_mib=128,
                           accelerator=accelerator, hard_disks=[], iso_readonly=True,
                           boot_timeout_seconds=90)

    def connect(self):
        deadline = time.monotonic()+10
        while not self.monitor.exists():
            assert self.process.poll() is None, 'QEMU exited before monitor startup'
            assert time.monotonic() < deadline, 'Monitor startup timed out'
            time.sleep(.05)

    def hmp(self, command):
        def prompt(sock):
            response = bytearray()
            while b'(qemu)' not in response:
                part = sock.recv(65536)
                if not part:
                    raise RuntimeError('QEMU monitor closed')
                response.extend(part)
            return bytes(response)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(3)
            sock.connect(str(self.monitor))
            prompt(sock)
            sock.sendall((command+'\n').encode())
            response = prompt(sock)
        with (self.output/'monitor-commands.jsonl').open('a') as log:
            log.write(json.dumps(dict(seconds=round(time.monotonic()-self.start, 3),
                                      command=command,
                                      response=response.decode(errors='replace')))+'\n')
        return response

    def offset(self):
        return self.serial.stat().st_size if self.serial.exists() else 0

    def data(self, offset=0):
        return normalize(self.serial.read_bytes()[offset:]) if self.serial.exists() else b''

    def wait(self, marker, offset=0, timeout=30):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if marker in self.data(offset):
                return
            assert self.process.poll() is None, 'QEMU exited while waiting for guest'
            time.sleep(.1)
        raise AssertionError(f'Timed out waiting for {marker!r}')

    def key(self, name):
        self.hmp('sendkey '+name+' 40')
        time.sleep(.12)

    def text(self, command):
        for character in command:
            self.key('spc' if character == ' ' else character)
        self.key('ret')

    def shot(self, name):
        path = self.output/(name+'.ppm')
        self.hmp(f'screendump "{path}"')
        image = Image.open(path).convert('RGB')
        image.save(path.with_suffix('.png'))
        return image

    def settled_desktop(self, name, safe=False):
        # READY reports the guest state. Observe actual settled pixels as well:
        # the VGA fallback paints directly and can finish after that marker.
        time.sleep(1)
        before = self.shot(name+'-sample')
        time.sleep(.4)
        after = self.shot(name)
        assert before.size == after.size, 'Desktop geometry changed while idle'
        width, height = after.size
        region = (0, 0, width, height-32)  # The task-strip clock may advance.
        assert ImageChops.difference(before.crop(region), after.crop(region)).getbbox() is None, \
            'Desktop pixels above the clock did not settle'
        if safe:
            assert after.size == (640, 480), f'Explicit SAFE mode changed: {after.size}'
        else:
            assert width >= 800 and height >= 600, f'Desktop below minimum: {after.size}'
        # Count the actual neutral panel surface; this rejects an empty desktop
        # or a partial title-only frame despite a matching readiness marker.
        workspace = after.crop((80, 30, width, height-32))
        panel_pixels = sum(count for count, (r, g, b) in workspace.getcolors(width*height)
                           if 190 <= r <= 225 and 190 <= g <= 225 and 190 <= b <= 225)
        assert panel_pixels > 12000, f'Window/control surface missing: {panel_pixels}'
        self.report[name] = dict(size=[width, height], stable_above_task_strip=True,
                                 neutral_panel_pixels=panel_pixels)

    def select_boot_entry(self, index):
        deadline = time.monotonic()+15
        dump = self.output/'grub-menu.bin'
        while time.monotonic() < deadline:
            self.hmp(f'pmemsave 0xb8000 4000 "{dump}"')
            if dump.exists():
                menu = dump.read_bytes()[::2].decode('cp437', errors='replace')
                if 'CiukiOS - Setup' in menu and 'Booting' not in menu:
                    self.key('up')  # Stop the timeout before selecting a row.
                    self.key('home')
                    for _ in range(index):
                        self.key('down')
                    self.shot('grub-selection')
                    self.key('ret')
                    self.report['grub_entry_index'] = index
                    return
            time.sleep(.08)
        raise AssertionError('GRUB Live/Setup menu was not offered')

    def run(self):
        self.connect()
        if self.case in ('live', 'muted'):
            if self.case == 'muted':
                self.select_boot_entry(6)
                self.wait(b'[BOOT-SESSION] MUTE', timeout=90)
            else:
                self.report['grub_entry_selection'] = 'default timeout'
            self.wait(b'[DESKTOP] READY', timeout=90)
            self.report['boot_ready_seconds'] = round(time.monotonic()-self.start, 3)
            self.settled_desktop('desktop')
            mark = self.offset(); self.key('f4')
            self.wait(b'CiukiOS SHELL ', mark)
            mark = self.offset(); self.text('comdemo')
            self.wait(b'COM demo via INT21h', mark)
            self.wait(b'CiukiOS SHELL ', mark)
            self.shot('comdemo-return')
            self.report['comdemo_executed_and_returned'] = True
            mark = self.offset(); self.text('exit')
            self.wait(b'[DESKTOP] READY', mark)
            self.settled_desktop('desktop-after-comdemo')
        elif self.case == 'setup':
            self.select_boot_entry(1)
            self.wait(b'[BOOT-SESSION] SETUP', timeout=90)
            self.wait(b'[SETUP-GUI] PAGE 00')
            assert b'[DESKTOP] READY' not in self.data(), 'Desktop opened before Setup'
            time.sleep(.8)
            setup = self.shot('setup-welcome')
            assert setup.size == (800, 600), f'Setup mode is {setup.size}'
            self.report['setup_size'] = list(setup.size)
            self.report['desktop_skipped_before_setup'] = True
            mark = self.offset(); self.key('esc')
            self.wait(b'CiukiOS SHELL ', mark)
            mark = self.offset(); self.text('exit')
            self.wait(b'[DESKTOP] READY', mark)
            self.settled_desktop('desktop-after-setup-cancel')
        elif self.case == 'dos':
            self.select_boot_entry(3)
            self.wait(b'[BOOT-SESSION] DOS', timeout=90)
            self.wait(b'CiukiOS SHELL ')
            assert b'[DESKTOP] READY' not in self.data(), 'Desktop opened before DOS'
            self.shot('dos-console')
            mark = self.offset(); self.text('exit')
            self.wait(b'[DESKTOP] READY', mark)
            self.settled_desktop('desktop-after-dos-entry')
        else:
            self.select_boot_entry(2)
            self.wait(b'[BOOT-SESSION] SAFE', timeout=90)
            self.wait(b'[DESKTOP] READY')
            self.settled_desktop('safe-desktop', safe=True)
        self.report['status'] = 'passed'

    def close(self):
        self.report['elapsed_seconds'] = round(time.monotonic()-self.start, 3)
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait(timeout=2)
        self.report['qemu_returncode_after_cleanup'] = self.process.returncode
        self.errors.close()
        self.monitor.unlink(missing_ok=True)
        if self.audio:
            from analyze_audio_wav import pcm_payload
            pcm, _ = pcm_payload(self.audio.read_bytes())
            self.report['startup_pcm_silent'] = not any(pcm)
            if any(pcm):
                self.report.update(status='failed', error='Muted boot emitted PCM audio')
        (self.output/'serial.normalized.log').write_bytes(self.data())
        (self.output/'result.json').write_text(json.dumps(self.report, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-sha256', required=True)
    parser.add_argument('--case', choices=('all', 'live', 'setup', 'dos', 'safe', 'muted'), default='all')
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    iso, output = args.iso.resolve(), args.output.resolve()
    assert not output.exists(), 'Use a fresh output directory to preserve earlier evidence'
    before = digest(iso)
    assert before == args.expect_sha256, f'Unexpected ISO SHA-256: {before}'
    output.mkdir(parents=True)
    report = dict(iso=str(iso), iso_sha256_before=before, status='running', cases={})
    failure = None
    try:
        cases = ('live', 'setup', 'dos', 'safe', 'muted') if args.case == 'all' else (args.case,)
        for case in cases:
            vm = Session(iso, output/case, case, args.accel)
            try:
                vm.run()
            except Exception as exc:
                vm.report.update(status='failed', error=str(exc))
                if vm.process.poll() is None and vm.monitor.exists():
                    vm.shot('failure')
                    (vm.output/'registers.log').write_bytes(vm.hmp('info registers'))
                failure = failure or str(exc)
            finally:
                vm.close()
                report['cases'][case] = vm.report
                if vm.report['status'] == 'failed':
                    failure = failure or vm.report.get('error', 'Session failed')
                print(json.dumps(vm.report, indent=2), flush=True)
        report['status'] = 'failed' if failure else 'passed'
    finally:
        report['iso_sha256_after'] = digest(iso)
        report['iso_unchanged'] = report['iso_sha256_after'] == before
        (output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    assert report['iso_unchanged'], 'Original read-only ISO changed'
    if failure:
        raise SystemExit(failure)


if __name__ == '__main__':
    main()
