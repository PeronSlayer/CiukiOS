"""Actual compiled fixtures -> H1 services -> independent production observer.

Only completed PE flows reach probe publication. This driver never interprets
NSIS or INF instructions, and never fills a success manifest itself.
"""
# SPDX-License-Identifier: MIT
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import struct
from . import host_services as h, registry_wire as reg
from scripts.win32 import census, installer_fixture as fixture


class FixtureDriver:
    def __init__(self, root, library, program, fixtures):
        self.service = h.Services(root, library, program)
        self.fixtures = Path(fixtures)
        self.expected = json.loads((fixture.SOURCE / 'expected.json').read_text())
        self.records, self.outputs = [], []
        self.paths = self.service.session()
        self.registry = self.service.registry
        self.registry._probe_reset()
        target = self.service.root / 'C/System/Win32/Fixtures'
        for name in self.expected['artifacts']:
            shutil.copyfile(self.fixtures / name, target / name)
        self.base = 'C:\\System\\Win32\\Fixtures\\'
        self.installed = self.service.root / 'C/Program Files/Ciuki Installer Fixture'

    def close(self):
        self.service.close()

    def execute(self, name, args='', expected=0):
        image = name if ':' in name else self.base + name
        code, output = self.service.run(image, args)
        self.outputs.append(output)
        if code != expected or f'TERMINATE {expected}\n' not in output:
            raise AssertionError(f'{image}: expected {expected}, got {code}\n{output[-12000:]}')
        return output

    def checkpoint(self, stage, code=0):
        out = C.create_string_buffer(4096)
        with self.registry.lock:
            accepted = self.registry._probe_checkpoint(self.registry.ctx, stage, code, out, len(out))
        if not accepted:
            raise AssertionError(f'production probe rejected stage {stage}')
        text = out.value.decode()
        self.records.append(text)
        # Fail immediately as well as at the final publisher.
        if self.registry.observe(stage):
            raise AssertionError(f'registry manifest at stage {stage}')

    def rpc(self, op, key, path='', name='', data=b'', type_=0):
        error, raw = self.registry.exchange(self.paths.pid, reg.request(op, key, path, name, data, type_), 65536)
        if error or len(raw) < 40:
            raise AssertionError(f'registry transport {error}')
        reply = struct.unpack_from('<10I', raw)
        if reply[1]:
            raise AssertionError(f'registry operation {op}: {reply[1]}')
        return reply, raw[40 + reply[8]:]

    def check_install(self):
        for name in self.expected['install']['files']:
            path = self.installed / name
            if not path.is_file(): raise AssertionError(f'file absent: {path.name}')
        for name in ('payload.txt', 'child.exe'):
            actual = hashlib.sha256((self.installed / name).read_bytes()).hexdigest()
            expected = self.expected['payload_sha256'] if name == 'payload.txt' else self.expected['artifacts'][name]
            if actual != expected: raise AssertionError(f'installed checksum: {name}')
        image = census.inspect((self.installed / 'uninstall.exe').read_bytes())
        if image['machine'] != 'i386': raise AssertionError('uninstaller PE')
        for name, target in self.expected['install']['links'].items():
            raw = self.paths.path(name).read_bytes()
            if len(raw) < 40 or raw[:8] != b'CIUKLNK3': raise AssertionError('link header')
            sizes = struct.unpack_from('<5I', raw, 16)
            if 40 + sum(sizes) != len(raw): raise AssertionError('link bounds')
            start = 40 + sizes[0]
            if raw[start:start + sizes[1]].decode() != target: raise AssertionError('link target')

    def chain(self, name):
        output = self.execute(name)
        if name == 'infchain.exe':
            expected = self.expected['inf']
            payload = self.service.root / 'C/Program Files/Ciuki INF Fixture/payload.txt'
            if hashlib.sha256(payload.read_bytes()).hexdigest() != expected['payload_sha256']:
                raise AssertionError('INF CopyFiles checksum')
            reply, _ = self.rpc(reg.OPEN, reg.HKLM, 'Software\\CiukiOS\\InstallerFixture')
            key = reply[2]
            try:
                value, data = self.rpc(reg.QUERY, key, name='Installed')
                if value[4] != 4 or data != struct.pack('<I', 1): raise AssertionError('INF AddReg DWORD')
                _, raw = self.registry.exchange(self.paths.pid, reg.request(reg.QUERY, key, name='Obsolete'), 1024)
                if struct.unpack_from('<I', raw, 4)[0] != 2: raise AssertionError('INF DelReg')
            finally: self.rpc(reg.CLOSE, key)
            text = (self.service.root / 'C/WINDOWS/fixture.ini').read_text()
            if '[Fixture]' not in text or 'Installed=1' not in text: raise AssertionError('INF UpdateInis')
        else:
            if any((self.service.root / 'C/WINDOWS/TEMP').iterdir()):
                raise AssertionError('self-extractor left a temporary tree')
        if not any(row[0] == 'wait' for row in self.service.trace):
            raise AssertionError('no actual child wait')
        if not any(row[0] == 'write' and row[1].endswith('child.done') for row in self.service.trace):
            raise AssertionError('child marker was not written')
        return output

    def nsis(self):
        self.execute(fixture.NAME, '/S /CANCEL', expected=1)
        self.checkpoint(1, 1)
        if self.installed.exists(): raise AssertionError('cancel left partial tree')
        if not any(row[0] == 'write' and row[1].endswith('payload.txt') for row in self.service.trace):
            raise AssertionError('cancel happened before partial extraction')
        self.execute(fixture.NAME, '/S')
        self.check_install(); self.checkpoint(2)
        self.execute(fixture.NAME, '/S /RESTART')
        self.check_install(); self.checkpoint(3)
        self.service.restart(lambda: self.checkpoint(4))
        self.checkpoint(5)
        marker = self.service.root / 'C/WINDOWS/TEMP/CiukiInstallerFixture.restarted'
        if marker.read_bytes() != b'child completed\r\n': raise AssertionError('RunOnce child completion')
        temporary = self.expected['uninstall']['temporary_copy']
        shutil.copyfile(self.installed / 'uninstall.exe', self.paths.path(temporary))
        self.execute(temporary, '/S _?=C:\\Program Files\\Ciuki Installer Fixture')
        self.checkpoint(6)
        self.service.restart(lambda: self.checkpoint(7))
        self.checkpoint(8)
        out = C.create_string_buffer(4096)
        with self.registry.lock:
            used = self.registry._probe_publish(out, len(out))
        if not used: raise AssertionError('probe publication failed')
        text = out.value.decode()
        self.records.append(text)
        if text.count('errors=0 manifest=1') != 4: raise AssertionError(text)
        return ''.join(self.records)
