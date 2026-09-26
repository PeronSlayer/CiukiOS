#!/usr/bin/env python3
"""Inject real QEMU NMIs at production LFB mode-switch instruction boundaries.

The 16 KiB boot probe uses a relocated real-mode IVT, distinctive registers,
and TF/IF/DF plus arithmetic flags. Its own real NMI/debug handlers reject
delivery while protected mode is active or while descriptor tables differ.
The production guard is included unchanged. This is a CPU transition test,
not physical GPU, CMOS, A20-driver or timing qualification.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000


class Remote:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(str(path))

    def packet(self, value):
        data = value.encode()
        self.sock.sendall(b'$' + data + b'#' + f'{sum(data)&255:02x}'.encode())
        while True:
            prefix = self.sock.recv(1)
            if not prefix:
                raise RuntimeError('QEMU closed GDB connection')
            if prefix == b'$':
                break
        data = bytearray()
        while (value := self.sock.recv(1)) != b'#':
            if not value:
                raise RuntimeError('QEMU closed GDB connection')
            data.extend(value)
        self.sock.recv(2)
        self.sock.sendall(b'+')
        return data.decode()

    def breakpoint(self, address, enable=True):
        assert self.packet(f'{"Z" if enable else "z"}0,{address:x},1') == 'OK'

    def until(self, address):
        self.breakpoint(address)
        result = self.packet('c')
        assert result.startswith(('T05', 'S05')), result
        self.breakpoint(address, False)

    def write_word(self, address, value):
        assert self.packet(f'M{address:x},2:{struct.pack("<H",value).hex()}') == 'OK'

    def resume(self):
        self.sock.sendall(b'$c#63')


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(str(path))
        self.file = self.sock.makefile('rb')
        json.loads(self.file.readline())
        self.command('qmp_capabilities')

    def command(self, name, arguments=None):
        message = {'execute': name}
        if arguments:
            message['arguments'] = arguments
        self.sock.sendall(json.dumps(message).encode() + b'\n')
        while True:
            response = json.loads(self.file.readline())
            if 'return' in response:
                return response['return']
            if 'error' in response:
                raise RuntimeError(response)


def run_case(image, symbols, output, name, address=None, closing=False):
    record = {'case': name, 'passed': False}
    log = output / f'{name}.log'
    with tempfile.TemporaryDirectory(prefix='ciuki-nmi-') as temp:
        temp = Path(temp)
        command = ['qemu-system-i386', '-machine', 'pc,accel=tcg', '-cpu', 'pentium3',
                   '-m', '16', '-drive', f'file={image},format=raw,if=floppy',
                   '-boot', 'a', '-display', 'none', '-serial', 'none', '-monitor', 'none',
                   '-no-reboot', '-debugcon', f'file:{log}',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                   '-S', '-gdb', f'unix:{temp}/gdb.sock,server=on,wait=off',
                   '-qmp', f'unix:{temp}/qmp.sock,server=on,wait=off']
        with (output/f'{name}.stderr').open('wb') as stderr:
            process = subprocess.Popen(command, stderr=stderr)
            try:
                deadline = time.monotonic() + 10
                while not (temp/'gdb.sock').exists() or not (temp/'qmp.sock').exists():
                    assert process.poll() is None, 'QEMU exited during startup'
                    assert time.monotonic() < deadline, 'QEMU did not create sockets'
                    time.sleep(.02)
                remote, qmp = Remote(temp/'gdb.sock'), QMP(temp/'qmp.sock')
                remote.until(symbols['ready'])
                if address is not None:
                    points = address if isinstance(address, list) else [address]
                    remote.write_word(symbols['expected_nmi'], len(points))
                    if closing:
                        remote.until(symbols['entered'])
                    record['injections'] = []
                    for index, point in enumerate(points):
                        remote.until(point)
                        state = qmp.command('human-monitor-command', {'command-line': 'info registers'})
                        (output/f'{name}-{index}.registers').write_text(state)
                        cr0 = re.search(r'CR0=([0-9a-fA-F]+)', state)[1]
                        record['protected'] = bool(int(cr0, 16) & 1)
                        record['injections'].append({'cr0': cr0, 'instruction': f'{point:08x}'})
                        qmp.command('inject-nmi')
                remote.until(symbols['exited'])
                final_state = qmp.command('human-monitor-command', {'command-line': 'info registers'})
                (output/f'{name}.restored-registers').write_text(final_state)
                es = re.search(r'ES\s*=([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', final_state)
                assert es and tuple(int(value, 16) for value in es.groups()) == (0x3456, 0x34560, 0xFFFF), final_state
                record['restored_es_limit'] = '0000ffff'
                remote.resume()
                process.wait(timeout=10)
                assert process.returncode == 33, (name, process.returncode)
                text = log.read_text()
                assert text == 'LFB GUARD PASS\n', (name, text)
                record['passed'] = True
                remote.sock.close()
                qmp.file.close()
                qmp.sock.close()
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', help='run one case by its name')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name in ('guard', 'boot'):
        subprocess.run(['nasm', '-f', 'bin', f'scripts/fixtures/vbe_burst_{name}.asm',
                        '-o', str(output/f'{name}.bin'), '-l', str(output/f'{name}.lst')],
                       cwd=ROOT, check=True)
    binary = (output/'guard.bin').read_bytes()
    assert len(binary) <= 17*512
    image = output/'probe.img'
    image.write_bytes((output/'boot.bin').read_bytes()+binary+bytes(1474560-512-len(binary)))
    symbols = {}
    cursor = binary.index(b'LFBG')+4
    while (address := struct.unpack_from('<H', binary, cursor)[0]):
        end = binary.index(0, cursor+2)
        symbols[binary[cursor+2:end].decode()] = BASE+address
        cursor = end+1
    start, end = symbols['vc_fb_limits'], symbols['vc_fb_nmi_pm']
    path = output/'switch.bin'
    path.write_bytes(binary[start-BASE-0x100:end-BASE-0x100])
    disassembly = subprocess.check_output(['ndisasm', '-b', '16', '-o', str(start), str(path)], text=True)
    (output/'switch.txt').write_text(disassembly)
    addresses = [int(line.split()[0], 16) for line in disassembly.splitlines()]
    cases = [('baseline', None, False)]
    cases += [(f'{phase}-{index:02d}', address, phase=='leave')
              for phase in ('enter', 'leave') for index, address in enumerate(addresses)]
    cases += [('multiple-enter', [addresses[6], addresses[10]], False),
              ('multiple-leave', [addresses[6], addresses[10]], True)]
    records = []
    for name, address, closing in cases:
        if args.case and args.case != name:
            continue
        record = run_case(image, symbols, output, name, address, closing)
        records.append(record)
        print(f'PASS {name}: {"protected" if record.get("protected") else "real"} mode', flush=True)
        (output/'results.json').write_text(json.dumps(records, indent=2)+'\n')
    assert records, 'no test cases selected'


if __name__ == '__main__':
    main()
