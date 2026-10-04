#!/usr/bin/env python3
"""Exercise shared display settings through the real shell and setup UI."""
import argparse
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import uuid


class VM:
    def __init__(self, disk, output, qemu_args=(), memory=512):
        output = Path(output)
        output.mkdir(parents=True, exist_ok=True)
        self.output = output
        self.serial = output / 'serial.log'
        self.sock = Path(tempfile.gettempdir()) / f'ciukios-display-{uuid.uuid4().hex}.sock'
        self.err = (output / 'qemu.stderr.log').open('w')
        qemu_args = list(qemu_args)
        audio_selected = any(arg in ('-audiodev', '-audio', '-soundhw') for arg in qemu_args)
        audio_selected |= any(arg.lower().startswith(('sb16', 'adlib', 'ac97'))
                              for arg in qemu_args)
        network_selected = any(arg in ('-netdev', '-nic', '-net') for arg in qemu_args)
        network_selected |= any(arg.lower().startswith(('ne2k_', 'e1000', 'rtl8139'))
                                for arg in qemu_args)
        full_devices = []
        if not audio_selected:
            full_devices.extend([
                '-audiodev', f'wav,id=ciuksnd0,path={output / "full-audio.wav"}',
                '-device', 'sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=ciuksnd0',
                '-device', 'adlib,audiodev=ciuksnd0',
            ])
        if not network_selected:
            full_devices.extend([
                '-netdev', 'user,id=ciuknet0',
                '-device', 'ne2k_isa,netdev=ciuknet0,irq=3,iobase=0x300,mac=52:54:00:12:34:56',
            ])
        self.process = subprocess.Popen([
            'qemu-system-i386', '-accel', 'kvm', '-machine', 'pc,vmport=off,i8042=on',
            '-cpu', 'pentium3', '-m', str(memory), '-drive', f'file={disk},format=raw,if=ide',
            '-boot', 'c', '-display', 'none', '-serial', f'file:{self.serial}',
            '-monitor', f'unix:{self.sock},server,nowait', '-no-reboot', '-no-shutdown',
            *full_devices, *qemu_args,
        ], stdout=subprocess.DEVNULL, stderr=self.err)
        deadline = time.monotonic() + 10
        while not self.sock.exists():
            assert time.monotonic() < deadline, 'monitor did not start'
            time.sleep(.05)

    def hmp(self, command):
        with socket.socket(socket.AF_UNIX) as conn:
            conn.settimeout(5)
            conn.connect(str(self.sock))
            banner = b''
            while b'(qemu)' not in banner:
                banner += conn.recv(4096)
            conn.sendall((command + '\n').encode())
            result = b''
            while b'(qemu)' not in result:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                result += chunk
            return result

    def key(self, key):
        self.hmp(f'sendkey {key} 30')
        time.sleep(.06)

    def text(self, command):
        for ch in command:
            if ch.isupper():
                key = 'shift-' + ch.lower()
            else:
                key = {' ': 'spc', '.': 'dot', '\\': 'backslash',
                       '-': 'minus', '/': 'slash', ':': 'shift-semicolon'}.get(ch, ch)
            self.key(key)
        self.key('ret')

    def offset(self):
        return self.serial.stat().st_size if self.serial.exists() else 0

    def wait(self, pattern, offset=0, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.serial.exists():
                result = subprocess.check_output([
                    'scripts/serial_log_normalize.py', '--offset', str(offset), str(self.serial)])
                if pattern.encode() in result:
                    return
                # Legacy console tests explicitly request a DOS prompt. The
                # shipping shell now boots its desktop; enter DOS once through
                # the public F4 shortcut. Native desktop tests opt out.
                if (pattern.startswith('CiukiOS SHELL ')
                        and getattr(self, 'auto_enter_dos', True)
                        and not getattr(self, '_entered_dos', False)
                        and b'[DESKTOP] READY' in result):
                    self._entered_dos = True
                    self.key('f4')
            time.sleep(.1)
        raise AssertionError(f'missing serial marker: {pattern}')

    def shot(self, name, size=None):
        path = self.output / (name + '.ppm')
        path.unlink(missing_ok=True)
        self.hmp(f'screendump {path}')
        deadline = time.monotonic() + 5
        while not path.exists() or path.stat().st_size < 64:
            assert time.monotonic() < deadline, 'screenshot was not written'
            time.sleep(.05)
        data = path.read_bytes().split(maxsplit=4)
        assert data[0] == b'P6'
        actual = int(data[1]), int(data[2])
        if size:
            assert actual == size, f'{name}: expected {size}, got {actual}'
        return path

    def command(self, command, marker=None, timeout=30):
        offset = self.offset()
        self.text(command)
        if marker:
            self.wait(marker, offset, timeout)
        self.wait('CiukiOS SHELL C:\\APPS>', offset, timeout)
        return offset

    def close(self):
        try:
            self.hmp('quit')
        except (OSError, TimeoutError):
            pass
        self.process.wait(timeout=10)
        self.err.close()
        self.sock.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/full/qemu-display-profile'))
    parser.add_argument('--shell', type=Path)
    parser.add_argument('--setup', type=Path)
    parser.add_argument('--initial', choices=('TEXT', '0640', '0800', '1024'), default='TEXT')
    parser.add_argument('--boot-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / 'test.img'
    shutil.copyfile(args.image, disk)
    for source, target in ((args.shell, '::SYSTEM/SHELL.COM'),
                           (args.setup, '::SYSTEM/DRIVERS/VGASETUP.COM')):
        if source:
            subprocess.run(['mcopy', '-o', '-i', str(disk), str(source), target], check=True)
    profile = output / 'DISPLAY.CFG'
    profile.write_text(args.initial)
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(profile),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    scroll = output / 'SCROLL.TXT'
    scroll.write_bytes(b''.join(f'screen line {i:02d}: abc123\r\n'.encode() for i in range(45)))
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(scroll), '::APPS/SCROLL.TXT'], check=True)

    def read_file(path):
        return subprocess.check_output(['mtype', '-i', str(disk), '::' + path])

    vm = VM(disk, output)
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>')
        size = {'TEXT': (720, 400), '0640': (640, 480),
                '0800': (800, 600), '1024': (1024, 768)}[args.initial]
        vm.shot('boot', size)
        vm.command('echo keyboard ready', 'keyboard ready\r\nCiukiOS SHELL')
        vm.shot('typed', size)
        print(f'[display-profile] PASS boot and keyboard at {size}', flush=True)
        if args.boot_only:
            return
        offset = vm.offset()
        vm.text('vgasetup')
        vm.wait('[VGASETUP] MENU READY', offset)
        vm.shot('menu')
        vm.key('1')
        vm.key('ret')
        vm.wait('[VGASETUP] PREVIEW', offset)
        time.sleep(.5)
        vm.shot('preview-640', (640, 480))
        offset = vm.offset()
        vm.key('esc')
        vm.wait('[VGASETUP] MENU READY', offset)
        assert read_file('SYSTEM/VIDEO/DISPLAY.CFG') == args.initial.encode()
        print('[display-profile] PASS 640x480 preview and ESC without saving', flush=True)
        offset = vm.offset()
        vm.key('2')
        vm.key('ret')
        vm.wait('[VGASETUP] PREVIEW', offset)
        time.sleep(1)
        vm.shot('preview-800', (800, 600))
        # First let the preview expire. Neither shared setting may be changed.
        previous_ini = read_file('WINDOWS/SYSTEM.INI')
        vm.wait('Preview cancelled', offset, 20)
        vm.wait('[VGASETUP] MENU READY', offset)
        assert read_file('SYSTEM/VIDEO/DISPLAY.CFG') == args.initial.encode()
        assert read_file('WINDOWS/SYSTEM.INI') == previous_ini
        print('[display-profile] PASS preview timeout preserves both settings', flush=True)
        offset = vm.offset()
        vm.key('ret')
        vm.wait('[VGASETUP] PREVIEW', offset)
        time.sleep(.5)
        offset = vm.offset()
        vm.key('ret')
        vm.wait('Shared resolution saved', offset)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset)
        vm.shot('shell-800', (800, 600))
        vm.text('echo impostazione applicata')
        vm.wait('impostazione applicata\r\nCiukiOS SHELL', offset)
        time.sleep(.5)
        vm.shot('shell-800-typed', (800, 600))
        assert read_file('SYSTEM/VIDEO/DISPLAY.CFG') == b'0800'
        assert b'Width=800' in read_file('WINDOWS/SYSTEM.INI')
        print('[display-profile] PASS menu, preview, shared commit and shell', flush=True)
        vm.command('type scroll.txt', 'screen line 44: abc123', timeout=120)
        shot = vm.shot('scrolled-800', (800, 600))
        pixels = shot.read_bytes().split(maxsplit=4)[4]
        lower = pixels[800 * 480 * 3:800 * 590 * 3]
        assert sum(lower[i:i+3] != b'\0\0\0' for i in range(0, len(lower), 3)) > 100
        vm.command('cls')
        vm.shot('cleared-800', (800, 600))
        print('[display-profile] PASS scrolling across video banks and CLS', flush=True)
        vm.command('vidleave', '[VIDLEAVE] MODE13')
        vm.shot('returned-800', (800, 600))
        vm.command('vgasetup set 1024', 'Shared resolution saved')
        vm.shot('shell-1024', (1024, 768))
        assert read_file('SYSTEM/VIDEO/DISPLAY.CFG') == b'1024'
        assert b'Width=1024' in read_file('WINDOWS/SYSTEM.INI')
        vm.close()
        reboot = output / 'reboot'
        reboot.mkdir(exist_ok=True)
        vm = VM(disk, reboot)
        vm.wait('CiukiOS SHELL C:\\APPS>')
        vm.shot('persistent-1024', (1024, 768))
        vm.command('echo reboot ready', 'reboot ready\r\nCiukiOS SHELL')
        print('[display-profile] PASS native child return and 1024x768 persistence', flush=True)
        # Force the second rename in the two-file transaction to fail after
        # SYSTEM.INI has been switched. Both old settings must be restored.
        vm.command('del \\system\\video\\display.bak')
        vm.command('mkdir \\system\\video\\display.bak')
        previous_ini = read_file('WINDOWS/SYSTEM.INI')
        vm.command('vgasetup set 800', 'profile not changed')
        assert read_file('SYSTEM/VIDEO/DISPLAY.CFG') == b'1024'
        assert read_file('WINDOWS/SYSTEM.INI') == previous_ini
        vm.shot('rollback-1024', (1024, 768))
        vm.command('rmdir \\system\\video\\display.bak')
        vm.command('vgasetup set safe', 'Shared resolution saved')
        vm.shot('recovery-text', (720, 400))
        assert read_file('SYSTEM/VIDEO/DISPLAY.CFG') == b'TEXT'
        assert b'display.drv=vga.drv' in read_file('WINDOWS/SYSTEM.INI')
        print('[display-profile] PASS failed commit rollback and VGA recovery', flush=True)
    except Exception:
        try:
            (output / 'failure-registers.log').write_bytes(vm.hmp('info registers'))
            vm.shot('failure')
        except OSError:
            pass
        raise
    finally:
        vm.close()


if __name__ == '__main__':
    main()
