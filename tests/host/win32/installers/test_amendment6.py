"""Amendment 6: cache closure, complete observations and durable recovery.

Native observer/service tests are not PE execution evidence. Actual compiled
NSIS/INF failure and rollback cases live in test_flow_fixtures.py.
"""
# SPDX-License-Identifier: MIT
import ctypes as C
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from scripts import build_nsis
from scripts.win32 import installer_fixture as fixture
from . import host_services as h, registry_wire as r
from .flow_driver import FixtureDriver

OUT = h.ROOT / 'build/host/win32/installers/amendment6'


class Amendment6Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library, _, _ = h.build_bridge(OUT)
        cls.fixtures = OUT / 'fixtures'
        cls.record = fixture.build(cls.fixtures)
        cls.expected = json.loads((fixture.SOURCE / 'expected.json').read_text())

    def test_license_notice_mismatch_refuses_fixture_before_compilation(self):
        with tempfile.TemporaryDirectory(dir=OUT) as name:
            root = Path(name)
            (root / 'third_party/nsis').mkdir(parents=True)
            (root / 'third_party/nsis/COPYING').write_bytes(b'wrong notice')
            with patch.object(fixture, 'ROOT', root):
                with self.assertRaisesRegex(ValueError, 'byte-identical.*COPYING'):
                    fixture.build(root / 'output')
                (root / 'third_party/nsis/COPYING').unlink()
                with self.assertRaisesRegex(ValueError, 'byte-identical.*COPYING'):
                    fixture.build(root / 'output')

    def test_cached_nsisdir_is_repaired_from_verified_archive(self):
        program = build_nsis.build()
        before = build_nsis.sha(program)
        record = json.loads((program.parent / 'build.json').read_text())
        data = Path(record['data'])
        expected = record['data_manifest']
        # Changed stub, missing include, extra resource and symlink. Each
        # cache hit must restore the complete set, not only makensis itself.
        cases = [('Stubs/zlib', 'corrupt'), ('Include/FileFunc.nsh', 'missing'),
                 ('Contrib/unexpected-resource', 'extra'), ('Include/FileFunc.nsh', 'symlink')]
        for relative, mutation in cases:
            with self.subTest(mutation=mutation):
                path = data / relative
                if mutation == 'missing': path.unlink()
                elif mutation == 'symlink':
                    path.unlink(); path.symlink_to('/etc/passwd')
                else: path.write_bytes(b'not a verified release resource')
                self.assertEqual(build_nsis.build(), program)
                self.assertEqual(build_nsis.data_manifest(data), expected)
                self.assertEqual(build_nsis.sha(program), before)
        record['data'] = '/outside-the-verified-closure'
        (program.parent / 'build.json').write_text(json.dumps(record))
        self.assertEqual(build_nsis.build(), program)
        self.assertEqual(json.loads((program.parent / 'build.json').read_text())['data'], str(data))

    def test_exact_deltas_observe_ordinary_windows_and_registry_writes(self):
        with tempfile.TemporaryDirectory(dir=OUT) as name:
            d = FixtureDriver(name, self.library, None, self.fixtures)
            try:
                d.state(1)
                ordinary = d.paths.path('C:/WINDOWS/ordinary.txt', True)
                ordinary.write_bytes(b'allowed Windows 98 write')
                with self.assertRaisesRegex(AssertionError, 'exact files delta'):
                    d.state(1)
                ordinary.unlink()
                key, _ = d.rpc(r.CREATE, r.HKLM, 'Software/Unrelated'.replace('/', chr(92)))
                d.rpc(r.SET, key[2], name='Permitted', data=b'\x01\0\0\0', type_=4)
                d.rpc(r.CLOSE, key[2])
                with self.assertRaisesRegex(AssertionError, 'exact registry delta'):
                    d.state(1)
                # The complete logical snapshot includes the unrelated key
                # and its typed value, not just Uninstall/RunOnce.
                self.assertIn('hklm\\software\\unrelated@permitted', d.registry.snapshot())
            finally:
                d.close()

    def test_observer_rejects_each_corrupt_content_fact(self):
        with tempfile.TemporaryDirectory(dir=OUT) as name:
            service = h.Services(name, self.library)
            session = service.session()
            try:
                installed = self.expected['install']['registry']['InstallLocation']
                rename = lambda target: ('[rename]\r\nNUL=' + target + '\r\n').encode()
                link_fields = [b'Ciuki Installer Fixture', (installed + '\\payload.txt').encode(),
                               b'', installed.encode(), (installed + '\\child.exe').encode()]
                link = b'CIUKLNK3' + struct.pack('<Ii5II', 1, 0, *map(len, link_fields), 0) + b''.join(link_fields)
                rows = [
                    (0, installed + '/payload.txt', (fixture.SOURCE / 'payload.txt').read_bytes()),
                    (1, installed + '/child.exe', (self.fixtures / 'child.exe').read_bytes()),
                    (2, installed + '/uninstall.exe', fixture.uninstaller_bytes((self.fixtures / fixture.NAME).read_bytes())),
                    (3, 'C:/WINDOWS/Desktop/Ciuki Installer Fixture.lnk', link),
                    (4, 'C:/WINDOWS/Start Menu/Programs/Ciuki Installer Fixture/Readme.lnk', link),
                    (5, self.expected['uninstall']['temporary_copy'], fixture.uninstaller_bytes((self.fixtures / fixture.NAME).read_bytes())),
                    (6, self.expected['restart']['delete'], b'authored restart deletion\r\n'),
                    (7, self.expected['restart']['completion'], b'child completed\r\n'),
                    (8, 'C:/WINDOWS/WININIT.INI', rename(self.expected['restart']['delete'])),
                    (9, 'C:/WINDOWS/WININIT.BAK', rename(self.expected['restart']['delete'])),
                    (10, self.expected['restart']['journal'], b'CIUKIRST 1\nphase=1\nflags=2\nline=0\nhanded=0\n')]
                def contents(stage):
                    reg = service.registry
                    reg._probe_reset()
                    out = C.create_string_buffer(4096)
                    for step in range(1, stage + 1):
                        self.assertEqual(reg._probe_checkpoint(reg.ctx, step, 1 if step == 1 else 0, out, len(out)), 1)
                    fields = dict(w.split('=', 1) for w in out.value.decode().split() if '=' in w)
                    return int(fields['contents'], 16)
                for bit, path, valid in rows:
                    with self.subTest(path=path):
                        target = session.path(path)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(valid)
                        self.assertTrue(contents(3) & (1 << bit))
                        mutations = [b'stale or corrupt']
                        if bit in (1, 2, 5):
                            changed = bytearray(valid); changed[len(changed)//2] ^= 1
                            mutations += [bytes(changed), valid + b'x', valid[:-1]]
                        if bit in (8, 9):
                            operand = self.expected['restart']['delete']
                            mutations += [(';[rename]\r\nNUL=' + operand + '\r\n').encode(),
                                          ('[rename]\r\n[Other]\r\nNUL=' + operand + '\r\n').encode(),
                                          ('[rename]\r\n;NUL=' + operand + '\r\n').encode(), valid + valid]
                            # Amendment 7: the complete instruction set is
                            # closed, including sections after the valid key.
                            padded = valid + b';' + b' ' * (4096 - len(valid) - 3) + b'\r\n'
                            target.write_bytes(padded)
                            self.assertTrue(contents(3) & (1 << bit))  # exactly at the buffer limit, EOF
                            extra = b'[DIRNUL]\r\nC:\\unrelated=0\r\n'
                            mutations += [valid + extra, extra + valid, valid + b'[Other]\r\n',
                                          b'NUL=outside-section\r\n' + valid,
                                          valid + b'unexpected=value\r\n', padded + extra, padded + b' ']
                        if bit == 10:
                            mutations += [valid.replace(b'line=0', b'line=2'), valid.replace(b'handed=0', b'handed=1'),
                                          valid.replace(b'flags=2', b'flags=0'), valid.replace(b'phase=1', b'phase=2'), valid.replace(b'phase=1\n', b'')]
                        for bad in mutations:
                            target.write_bytes(bad)
                            self.assertFalse(contents(3) & (1 << bit), (path, bad))
                        target.write_bytes(valid)
                # Every field of each link matters, including show/icon,
                # description, arguments and the working directory.
                target = session.path(rows[3][1])
                for offset in (8, 12, 36, 40, 40 + len(link_fields[0]), len(link) - 1):
                    bad = bytearray(link); bad[offset] ^= 1; target.write_bytes(bad)
                    self.assertFalse(contents(3) & 8)
            finally:
                service.close()

    def test_amendment7_stage_specific_instruction_sets_and_eof(self):
        with tempfile.TemporaryDirectory(dir=OUT) as name:
            service = h.Services(name, self.library)
            session = service.session()
            try:
                restart = self.expected['restart']['delete']
                uninstall = self.expected['uninstall']['temporary_copy']
                for stage in range(1, 9):
                    for retired in ((False, True) if stage == 7 else (False,)):
                        journal = session.path(self.expected['restart']['journal'])
                        if retired: journal.write_bytes(b'CIUKIRST 1\nphase=2\nflags=2\nline=1\nhanded=0\n')
                        elif journal.exists(): journal.unlink()
                        for bit, filename in ((8, 'WININIT.INI'), (9, 'WININIT.BAK')):
                            target = restart if (stage <= 5 if bit == 8 else stage <= 7 and not retired) else uninstall
                            valid = ('[Rename]\r\nNUL=' + target + '\r\n').encode()
                            padded = valid + b';' + b' ' * (4096 - len(valid) - 3) + b'\r\n'
                            instruction = b'[DIRNUL]\r\nC:\\unrelated=0\r\n'
                            cases = [(valid, True), (padded, True), (valid + instruction, False),
                                     (padded + instruction, False), (padded + b' ', False),
                                     (valid.replace(target.encode(), (uninstall if target == restart else restart).encode()), False)]
                            for raw, accepted in cases:
                                with self.subTest(stage=stage, retired=retired, file=filename, size=len(raw), accepted=accepted):
                                    session.path('C:/WINDOWS/' + filename).write_bytes(raw)
                                    service.registry._probe_reset()
                                    out = C.create_string_buffer(4096)
                                    for step in range(1, stage + 1):
                                        self.assertEqual(service.registry._probe_checkpoint(service.registry.ctx, step, 1 if step == 1 else 0, out, len(out)), 1)
                                    fields = dict(w.split('=', 1) for w in out.value.decode().split() if '=' in w)
                                    self.assertEqual(bool(int(fields['contents'], 16) & (1 << bit)), accepted)
            finally:
                service.close()

    def test_restart_interruption_and_recovery_of_command_consumption_on_new_service(self):
        # No PE claim: stop around real filesystem/registry persistence,
        # recreate w32srv and assert the command handoff's at-most-once rule.
        for boundary in ('before_begin', 'journal_persisted', 'wininit_tick', 'retire_failed',
                         'retired_journal_failed', 'wininit_retired', 'consumed_journal_failed', 'runonce_consumed'):
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory(dir=OUT) as name:
                service = h.Services(name, self.library)
                session = service.session()
                def rpc(op, key, path='', name_='', data=b'', type_=0):
                    error, raw = service.registry.exchange(session.pid, r.request(op, key, path, name_, data, type_), 4096)
                    self.assertEqual(error, 0)
                    reply = struct.unpack_from('<10I', raw)
                    self.assertEqual(reply[1], 0)
                    return reply[2]
                key = rpc(r.CREATE, r.HKLM, 'Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce')
                command = '"C:\\WINDOWS\\TEMP\\recovery-child.exe"'
                rpc(r.SET, key, name_='Recovery', data=(command + '\0').encode('utf-16le'), type_=1)
                rpc(r.CLOSE, key)
                root = Path(name) / 'C/WINDOWS'
                victim = root / 'TEMP/victim'
                victim.write_bytes(b'pending')
                ini = b'[rename]\r\nNUL=C:\\WINDOWS\\TEMP\\victim\r\n'
                (root / 'WININIT.INI').write_bytes(ini)
                old = service.registry
                consumed = False
                try:
                    if boundary != 'before_begin':
                        self.assertEqual(old._restart_begin(old.ctx), 0)
                        self.assertEqual((root / 'CIUKIRST.LOG').read_bytes(), b'CIUKIRST 1\nphase=1\nflags=2\nline=0\nhanded=0\n')
                    if boundary not in ('before_begin', 'journal_persisted'):
                        old._restart_tick(old.ctx)
                        self.assertFalse(victim.exists())
                    if boundary in ('retire_failed', 'retired_journal_failed'):
                        old._fault(old.ctx, 3 if boundary == 'retire_failed' else 1)
                        old._restart_tick(old.ctx)
                        self.assertEqual((root / 'WININIT.INI').exists(), boundary == 'retire_failed')
                    elif boundary in ('wininit_retired', 'consumed_journal_failed', 'runonce_consumed'):
                        old._restart_tick(old.ctx)
                        self.assertEqual(old._restart_phase(old.ctx), 2)
                        if boundary != 'wininit_retired':
                            if boundary == 'consumed_journal_failed': old._fault(old.ctx, 2)
                            out = C.create_string_buffer(2048)
                            self.assertEqual(old._restart_next(old.ctx, out, len(out)), 29 if boundary == 'consumed_journal_failed' else 0)
                            consumed = True
                    # Recreate the service without its normal TEMP teardown:
                    # this is an interrupted boot, not installer cleanup.
                    session.close(); old.close()
                    service = h.Services(name, self.library)
                    session = service.session()
                    reg = service.registry
                    self.assertEqual(reg._restart_begin(reg.ctx), 0)
                    for _ in range(16):
                        if reg._restart_phase(reg.ctx) != 1: break
                        reg._restart_tick(reg.ctx)
                    self.assertEqual(reg._restart_phase(reg.ctx), 2)
                    self.assertFalse(victim.exists())
                    self.assertEqual((root / 'WININIT.BAK').read_bytes(), ini)
                    out = C.create_string_buffer(2048)
                    self.assertEqual(reg._restart_next(reg.ctx, out, len(out)), 259 if consumed else 0)
                    if not consumed: self.assertEqual(out.value.decode(), command)
                    self.assertEqual(reg._restart_next(reg.ctx, out, len(out)), 259)
                    self.assertEqual(reg._restart_end(reg.ctx), 0)
                    self.assertFalse((root / 'CIUKIRST.LOG').exists())
                    self.assertFalse((root / 'CIUKIRST.TMP').exists())
                    service.assert_quiescent()
                finally:
                    service.close()
