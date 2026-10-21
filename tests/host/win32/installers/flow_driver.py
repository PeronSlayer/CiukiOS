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
from scripts.win32 import installer_fixture as fixture


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
        self.baseline = self.snapshot()

    def close(self):
        self.service.close()

    def execute(self, name, args='', expected=0):
        image = name if ':' in name else self.base + name
        code, output = self.service.run(image, args)
        self.outputs.append(output)
        if code != expected or f'TERMINATE {expected}\n' not in output:
            raise AssertionError(f'{image}: expected {expected}, got {code}\n{output[-12000:]}')
        return output

    def snapshot(self):
        files = {}
        for drive in ('C', 'D'):
            for path in (self.service.root / drive).rglob('*'):
                key = path.relative_to(self.service.root).as_posix()
                if path.is_symlink(): raise AssertionError('unexpected fixture symlink')
                files[key] = 'directory' if path.is_dir() else hashlib.sha256(path.read_bytes()).hexdigest()
        return {'files': files, 'registry': self.registry.snapshot()}

    def compare(self, state, baseline=None):
        baseline = baseline or self.baseline
        expected = {kind: baseline[kind] | {key.casefold() if kind == 'registry' else key: value
                    for key, value in state[kind].items()} for kind in ('files', 'registry')}
        actual = self.snapshot()
        for kind in expected:
            if actual[kind] != expected[kind]:
                changed = {key: (expected[kind].get(key), actual[kind].get(key))
                           for key in expected[kind].keys() | actual[kind].keys()
                           if expected[kind].get(key) != actual[kind].get(key)}
                raise AssertionError(f'exact {kind} delta: {changed}')

    def state(self, stage):
        self.compare(self.expected['states'][str(stage)])
        temporary = [p[len('C/WINDOWS/TEMP/'):] for p in self.expected['states'][str(stage)]['files']
                     if p.startswith('C/WINDOWS/TEMP/')]
        self.service.assert_quiescent(temporary)

    def checkpoint(self, stage, code=0):
        self.state(stage)
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
        for name, identity in self.expected['identities'].items():
            raw = (self.installed / name).read_bytes()
            if len(raw) != identity['size'] or hashlib.sha256(raw).hexdigest() != identity['sha256']:
                raise AssertionError(f'executable identity: {name}')
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
        self.state(name)
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

    def cancellation_cases(self):
        self.execute(fixture.NAME, '/S /CANCEL /LATE /RESTART', expected=1)
        self.state(1)
        if not any(row[0] == 'write' and row[1].endswith('uninstall.exe') for row in self.service.trace):
            raise AssertionError('late cancel never created the uninstaller')
        written = set()
        for raw in self.registry.trace:
            fields = struct.unpack_from('<10I', raw)
            if fields[1] == reg.SET:
                written.add(raw[40 + fields[7]:40 + fields[7] + fields[8] - 1].decode())
        if not set(self.expected['install']['registry']).union({'CiukiInstallerFixture'}) <= written:
            raise AssertionError(f'late cancel missed registry writes: {written}')
        self.execute(fixture.NAME, '/S')
        self.state(2)
        (self.installed / 'payload.txt').write_bytes(b'earlier installation, preserve these bytes')
        key, _ = self.rpc(reg.OPEN, reg.HKLM, self.expected['install']['uninstall_key'].split('\\', 1)[1])
        self.rpc(reg.SET, key[2], name='DisplayName', data=b'previous binary value', type_=3)
        self.rpc(reg.SET, key[2], name='EarlierValue', data=b'\x07\0\0\0', type_=4)
        self.rpc(reg.CLOSE, key[2])
        before = self.snapshot()
        for args in ('/S /CANCEL', '/S /CANCEL /LATE /RESTART'):
            self.execute(fixture.NAME, args, expected=1)
            self.compare({'files': {}, 'registry': {}}, before)
            self.service.assert_quiescent()

    def interrupted_restart(self, library, boundary):
        self.execute(fixture.NAME, '/S /RESTART')
        self.state(3)
        class Interrupted(Exception): pass
        def interrupt(event):
            if event == boundary: raise Interrupted()
        try:
            self.service.restart(interrupt=interrupt)
        except Interrupted:
            pass
        else: raise AssertionError('restart interruption was not reached')
        previous = self.service
        previous.finish_descendants()
        for session in previous.sessions: session.close()
        previous.registry.close()
        self.service = h.Services(previous.root, library, previous.program)
        self.paths = self.service.session()
        self.registry = self.service.registry
        self.service.restart()
        state = {kind: dict(values) for kind, values in self.expected['states']['5'].items()}
        if boundary == 'runonce_consumed':
            # Deletion is durable before handoff: interruption loses this
            # command and must not replay it on the reconstructed service.
            del state['files']['C/WINDOWS/TEMP/CiukiInstallerFixture.restarted']
        self.compare(state)
        temporary = [] if boundary == 'runonce_consumed' else ['CiukiInstallerFixture.restarted']
        self.service.assert_quiescent(temporary)
        expected = 0 if boundary == 'runonce_consumed' else 1
        executions = sum(row[0] == 'spawn' and row[1].endswith('child.exe')
                         for row in previous.trace + self.service.trace)
        if executions != expected: raise AssertionError(f'RunOnce executions: {executions}, expected {expected}')

    def original_uninstall(self):
        self.execute(fixture.NAME, '/S')
        self.state(2)
        start = len(self.service.trace)
        self.execute('C:\\Program Files\\Ciuki Installer Fixture\\uninstall.exe', '/S')
        self.service.finish_descendants()
        spawned = [row[1] for row in self.service.trace[start:] if row[0] == 'spawn']
        delegated = [p for p in spawned if p != 'C:\\Program Files\\Ciuki Installer Fixture\\uninstall.exe']
        if len(delegated) != 1 or delegated[0] not in (
                'C:\\WINDOWS\\TEMP\\~nsu.tmp\\Au_.exe', 'C:\\WINDOWS\\TEMP\\~nsuA.tmp\\Au_.exe'):
            raise AssertionError(f'original uninstaller did not copy/start/exit: {spawned}')
        image = delegated[0]
        directory = image.rsplit('\\', 1)[0]
        # The unchanged engine owns the copy/start/exit operation but reuses
        # its shared directory. The authored section cannot claim ownership
        # of that parent: only the delegated executable may be retired.
        ini = ('[Rename]\r\nNUL=' + image + '\r\n').encode()
        path = lambda text: text[0] + '/' + text[3:].replace('\\', '/')
        state = {'files': {path(directory): 'directory', path(image): self.expected['identities']['uninstall.exe']['sha256'],
                          'C/WINDOWS/WININIT.INI': hashlib.sha256(ini).hexdigest()}, 'registry': {}}
        self.compare(state)
        self.service.assert_quiescent((directory.rsplit('\\', 1)[1], directory.rsplit('\\', 1)[1] + '/Au_.exe'))
        self.service.restart()
        self.compare({'files': {path(directory): 'directory',
                               'C/WINDOWS/WININIT.BAK': hashlib.sha256(ini).hexdigest()}, 'registry': {}})
        self.service.assert_quiescent((directory.rsplit('\\', 1)[1],))

    def unrelated_uninstall(self, directory, populated):
        self.execute(fixture.NAME, '/S')
        self.state(2)
        parent = self.paths.path(directory)
        parent.mkdir()
        sentinel = b'pre-existing unrelated content'
        if populated: (parent / 'unrelated.txt').write_bytes(sentinel)
        image = directory + '\\copy.exe'
        shutil.copyfile(self.installed / 'uninstall.exe', self.paths.path(image))
        self.execute(image, '/S _?=C:\\Program Files\\Ciuki Installer Fixture')
        ini = ('[rename]\r\nNUL=' + image + '\r\n').encode()
        path = lambda text: text[0] + '/' + text[3:].replace('\\', '/')
        preserved = {path(directory): 'directory'}
        if populated: preserved[path(directory) + '/unrelated.txt'] = hashlib.sha256(sentinel).hexdigest()
        self.compare({'files': preserved | {path(image): self.expected['identities']['uninstall.exe']['sha256'],
                     'C/WINDOWS/WININIT.INI': hashlib.sha256(ini).hexdigest()}, 'registry': {}})
        self.service.restart()
        self.compare({'files': preserved | {'C/WINDOWS/WININIT.BAK': hashlib.sha256(ini).hexdigest()}, 'registry': {}})
        prefix = 'C/WINDOWS/TEMP/'
        self.service.assert_quiescent([key[len(prefix):] for key in preserved if key.startswith(prefix)])
