"""Transport/service tests, separate from actual compiled PE flow evidence."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import ctypes as C
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

from . import host_services as h
OUT = h.ROOT / 'build/host/win32/installers/services'


def wire(op, fields, strings=(), buffer=b''):
    sizes = {h.FILE: 18, h.PROCESS: 18, h.OBJECT: 18, h.WAIT: 14, h.SYSTEM: 12}
    values = [0] * sizes[op]
    values[:4] = [1, len(values) * 4, op, 0]
    for index, value in fields.items(): values[index] = value
    tail = bytearray()
    for index, text in strings:
        values[index] = len(values) * 4 + len(tail)
        tail += text.encode() + b'\0'
    if buffer:
        values[8 if op == h.FILE else 4] = len(values) * 4 + len(tail)
        tail += buffer
    return struct.pack('<' + 'I' * len(values), *values) + tail


def call(session, op, fields, strings=(), buffer=b''):
    status, raw = session.dispatch(1, wire(op, fields, strings, buffer), h.LIMIT)
    if status: raise RuntimeError(status)
    return struct.unpack_from('<' + 'I' * ({h.FILE: 18, h.PROCESS: 18, h.OBJECT: 18,
        h.WAIT: 14, h.SYSTEM: 12}[op]), raw), raw


class HostServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library, cls.flags, cls.sources = h.build_bridge(OUT)
        cls.child = OUT / 'native-transport-child'
        subprocess.run([*cls.flags, '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        h.HERE / 'service_child.c', '-o', cls.child], check=True)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=OUT)
        self.service = h.Services(self.directory.name, self.library, self.child)
        self.session = self.service.session()

    def tearDown(self):
        self.service.close()
        self.directory.cleanup()

    def test_files_errors_case_spaces_bounds_and_find(self):
        s = self.session
        created, _ = call(s, h.FILE, {4: 1, 10: 0xc0000000, 11: 7, 12: 1},
                          [(6, 'C:\\WINDOWS\\TEMP\\Long name.txt')])
        self.assertEqual(created[17], 0)
        handle = created[16]
        wrote, _ = call(s, h.FILE, {4: 3, 5: handle, 9: 7}, buffer=b'Ciuki\r\n')
        self.assertEqual(wrote[16:18], (7, 0))
        seek, _ = call(s, h.FILE, {4: 4, 5: handle})
        self.assertEqual(seek[14:18], (0, 0, 0, 0))
        read, raw = call(s, h.FILE, {4: 2, 5: handle, 9: 16}, buffer=bytes(16))
        self.assertEqual(read[16:18], (7, 0))
        self.assertEqual(raw[read[8]:read[8] + 7], b'Ciuki\r\n')
        duplicate, _ = call(s, h.FILE, {4: 1, 10: 0x40000000, 11: 7, 12: 1},
                            [(6, 'c:\\windows\\temp\\LONG NAME.TXT')])
        self.assertEqual(duplicate[17], 80)
        first, raw = call(s, h.FILE, {4: 17, 9: 832}, [(6, 'C:\\WINDOWS\\TEMP\\*.*')], bytes(832))
        self.assertEqual(first[17], 0)
        self.assertIn(b'Long name.txt\0', raw[first[8]:])
        self.assertEqual(call(s, h.FILE, {4: 18, 5: first[16], 9: 832}, buffer=bytes(832))[0][17], 18)
        self.service.disk_full = True
        self.assertEqual(call(s, h.FILE, {4: 3, 5: handle, 9: 1}, buffer=b'x')[0][17], 112)
        ro, _ = call(s, h.FILE, {4: 1, 10: 0x40000000, 12: 2}, [(6, 'D:\\ro.txt')])
        self.assertEqual(ro[17], 19)
        bad, _ = call(s, h.FILE, {4: 2, 5: handle, 8: 9000, 9: 10})
        self.assertEqual(bad[17], 87)
        self.assertEqual(call(s, h.OBJECT, {4: 3, 6: handle})[0][15], 0)
        self.assertEqual(call(s, h.OBJECT, {4: 3, 6: handle})[0][15], 6)

    def test_fixture_root_isolation_and_share_delete(self):
        s = self.session
        self.assertEqual(s.path('C:\\..\\..\\inside').parent, self.service.root / 'C')
        (self.service.root / 'C/escape').symlink_to(self.service.root.parent, target_is_directory=True)
        with self.assertRaises(PermissionError): s.path('C:\\escape\\outside')
        with self.assertRaises(PermissionError): s.path('\\\\host\\outside')
        with self.assertRaises(PermissionError): s.path('C:\\System\\Win32\\evil.dll', True)
        a, _ = call(s, h.FILE, {4: 1, 10: 0x40000000, 11: 0, 12: 2}, [(6, 'C:\\WINDOWS\\TEMP\\locked')])
        self.assertEqual(call(s, h.FILE, {4: 10}, [(6, 'C:\\WINDOWS\\TEMP\\locked')])[0][17], 32)
        call(s, h.OBJECT, {4: 3, 6: a[16]})
        self.assertEqual(call(s, h.FILE, {4: 10}, [(6, 'C:\\WINDOWS\\TEMP\\locked')])[0][17], 0)

    def test_actual_child_wait_query_close_and_transport(self):
        # Native protocol unit fixture: genuine process exit 23, not PE evidence.
        (self.service.root / 'C/WINDOWS/TEMP/child.exe').write_bytes(b'authored transport unit marker')
        a, _ = call(self.session, h.PROCESS, {}, [(4, 'C:\\WINDOWS\\TEMP\\child.exe'),
            (5, '"C:\\WINDOWS\\TEMP\\child.exe"'), (6, 'C:\\WINDOWS\\TEMP')])
        self.assertEqual(a[15], 0)
        process, thread = a[11:13]
        self.assertNotEqual(process, thread)
        query, _ = call(self.session, h.OBJECT, {4: 9, 6: process})
        self.assertEqual(query[5], 1)
        self.assertEqual(query[14], 259)
        poll, _ = call(self.session, h.WAIT, {5: 1, 8: 0}, buffer=struct.pack('<I', process))
        self.assertEqual(poll[11], 258)
        (self.service.root / 'C/WINDOWS/TEMP/transport-child.release').write_bytes(b'continue')
        waited, _ = call(self.session, h.WAIT, {5: 1, 8: 30000}, buffer=struct.pack('<I', process))
        self.assertEqual(waited[11:13], (0, 0))
        self.assertEqual(call(self.session, h.OBJECT, {4: 9, 6: process})[0][14], 23)
        self.assertEqual((self.service.root / 'C/WINDOWS/TEMP/transport-child.done').read_bytes(), b'child\n')
        self.assertEqual(call(self.session, h.OBJECT, {4: 9, 6: thread})[0][5], 2)
        for handle in (process, thread): self.assertEqual(call(self.session, h.OBJECT, {4: 3, 6: handle})[0][15], 0)
        self.assertEqual(self.service.children[-1].finish(10)[0], 23)

    def test_socket_framing_short_io_and_registry_production_model(self):
        parent, peer = socket.socketpair()
        worker = threading.Thread(target=self.session.serve, args=(parent,))
        worker.start()
        try:
            # W32R_OPEN HKLM/Software, by the same model used by the desktop.
            from .registry_wire import request, OPEN, CLOSE
            raw = request(OPEN, 0x80000002, 'Software')
            frame = struct.pack('<4I', h.MAGIC, 2, len(raw), 1024) + raw
            for byte in frame: h.send(peer, bytes([byte]))
            error, count = struct.unpack('<iI', h.receive(peer, 8))
            self.assertEqual(error, 0)
            reply = h.receive(peer, count)
            key = struct.unpack_from('<I', reply, 8)[0]
            self.assertNotEqual(key, 0)
            self.assertEqual(struct.unpack_from('<I', reply, 4)[0], 0)
            error, _ = self.service.registry.exchange(self.session.pid, request(CLOSE, key), 1024)
            self.assertEqual(error, 0)
        finally:
            peer.close(); worker.join(10)
        self.assertFalse(worker.is_alive())
        self.assertFalse(self.service.failures)

    def test_named_mutex_lifetime_and_missing_cmd_child(self):
        a, _ = call(self.session, h.OBJECT, {4: 1, 5: 4}, [(11, 'CiukiInstallerFixture')])
        other = self.service.session()
        b, _ = call(other, h.OBJECT, {4: 1, 5: 4}, [(11, 'ciukiinstallerfixture')])
        self.assertEqual(b[15], 183)
        call(self.session, h.OBJECT, {4: 3, 6: a[14]})
        call(other, h.OBJECT, {4: 3, 6: b[14]})
        c, _ = call(self.session, h.OBJECT, {4: 1, 5: 4}, [(11, 'CiukiInstallerFixture')])
        self.assertEqual(c[15], 0)
        # Cmd-less batch deletion is unsupported; never invoke a host shell.
        refused, _ = call(self.session, h.PROCESS, {}, [(4, 'C:\\WINDOWS\\COMMAND\\cmd.exe'),
                                                      (5, 'cmd.exe /c del self.exe')])
        self.assertEqual(refused[15], 2)
        self.assertFalse(self.service.children)

    def test_production_probe_reads_fixture_bytes_and_refuses_incomplete_publication(self):
        registry = self.service.registry
        directory = self.service.root / 'C/Program Files/Ciuki Installer Fixture'
        directory.mkdir()
        payload = directory / 'payload.txt'
        expected = (h.ROOT / 'tests/win32/fixtures/nsis/payload.txt').read_bytes()
        for content, mask in ((b'corrupt fixture', '0'), (expected, '1')):
            payload.write_bytes(content)
            out = C.create_string_buffer(4096)
            registry._probe_reset()
            self.assertEqual(registry._probe_checkpoint(registry.ctx, 1, 1, out, len(out)), 1)
            fields = dict(word.split('=', 1) for word in out.value.decode().split() if '=' in word)
            self.assertEqual(fields['contents'], mask)
            used = registry._probe_publish(out, len(out))
            self.assertGreater(used, 0)
            self.assertNotIn(b'errors=0 manifest=1', out.value)
            self.assertEqual(out.value.count(b'manifest=0'), 4)

    def test_copied_record_validation_and_metadata_roundtrip(self):
        self.assertEqual(self.session.dispatch(1, b'short', 4096), (-22, b''))
        malformed = wire(h.FILE, {4: 7, 6: 999999})
        with self.assertRaises(ValueError): self.session.dispatch(1, malformed, 4096)
        a, _ = call(self.session, h.FILE, {4: 1, 10: 0xc0000000, 11: 7, 12: 2},
                    [(6, 'C:\\WINDOWS\\TEMP\\metadata')])
        dates = (33, 0, 34, 35, 1)
        info = struct.pack('<III5HBB768s40s', 128, 0, 0, *dates, 0, 0, b'', b'')
        set_, _ = call(self.session, h.FILE, {4: 9, 5: a[16], 9: 832, 13: 7}, buffer=info)
        self.assertEqual(set_[17], 0)
        result, raw = call(self.session, h.FILE, {4: 7, 5: a[16], 9: 832}, buffer=bytes(832))
        self.assertEqual(struct.unpack_from('<5H', raw, result[8] + 12), dates)

    def test_native_h1_memory_and_inf_mapping_endpoint(self):
        binary = OUT / 'h1-endpoint-ubsan'
        subprocess.run([*self.flags, '-DW32_HOST_MODEL', '-no-pie', '-pthread',
            '-fsanitize=undefined', '-fno-sanitize-recover=all',
            '-I' + str(h.ROOT / 'tests/host/win32/adapter'),
            h.HERE / 'h1_endpoint_test.c', h.HERE / 'h1_services.c',
            h.ROOT / 'sdk/win32/services/bytes.c', '-o', binary], check=True)
        (self.service.root / 'C/WINDOWS/TEMP/endpoint.inf').write_bytes(
            b'[Version]\r\nSignature="$CHICAGO$"\r\n')
        self.service.program = binary
        code, output = self.service.run('C:\\WINDOWS\\TEMP\\endpoint.inf')
        self.assertEqual(code, 0, output)
        self.assertIn('PASS native H1 VM records and INF read-only view', output)
        print(output, end='')


class NativeBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _, flags, sources = h.build_bridge(OUT)
        cls.binaries = {}
        for name, sanitizer in (('ubsan', 'undefined'), ('asan', 'address,undefined')):
            binary = OUT / ('service-bridge-' + name)
            subprocess.run([*flags, '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all',
                            *sources, h.HERE / 'service_bridge_test.c', '-o', binary], check=True)
            cls.binaries[name] = binary

    def run_bridge(self, name):
        with tempfile.TemporaryDirectory(dir=OUT) as root:
            result = subprocess.run([self.binaries[name], root], capture_output=True, text=True, timeout=30)
        (OUT / ('bridge-' + name + '.log')).write_text(result.stdout + result.stderr)
        if name == 'asan' and 'LeakSanitizer has encountered a fatal error' in result.stderr:
            self.skipTest('LeakSanitizer cannot inspect threads in this sandbox; leak detection remains enabled')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS production bridge', result.stdout)

    def test_native_production_bridge_undefined_sanitizer(self):
        self.run_bridge('ubsan')

    def test_native_production_bridge_address_and_leak_sanitizers(self):
        self.run_bridge('asan')


if __name__ == '__main__': unittest.main()
