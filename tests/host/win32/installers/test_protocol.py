"""Actual C encoder/Python decoder conformance, without running a PE."""
# SPDX-License-Identifier: MIT
from contextlib import contextmanager
import errno
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

from . import host_services as h
from .test_host_services import wire

OUT = h.ROOT / 'build/host/win32/installers/protocol'


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library, flags, _ = h.build_bridge(OUT)
        cls.binary = OUT / 'h1-protocol-ubsan'
        subprocess.run([*flags, '-DW32_HOST_MODEL', '-no-pie', '-pthread',
            '-fsanitize=undefined', '-fno-sanitize-recover=all',
            '-I' + str(h.ROOT / 'tests/host/win32/adapter'),
            h.HERE / 'h1_protocol_test.c', h.HERE / 'h1_services.c',
            h.ROOT / 'sdk/win32/services/bytes.c', '-o', cls.binary], check=True)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=OUT)
        self.service = h.Services(self.directory.name, self.library, self.binary)

    def tearDown(self):
        try:
            self.service.close()
        finally:
            self.directory.cleanup()

    @contextmanager
    def connection(self):
        server, client = socket.socketpair()
        session = self.service.session()
        worker = threading.Thread(target=session.serve, args=(server,), daemon=True)
        worker.start()
        client.settimeout(2)
        try:
            yield session, client
        finally:
            client.close()
            worker.join(timeout=2)
            self.assertFalse(worker.is_alive(), session.diagnostic())

    def request(self, sock, kind, raw=b'', cap=h.LIMIT):
        h.send(sock, struct.pack('<4I', h.MAGIC, kind, len(raw), cap) + raw)
        return self.reply(sock)

    def reply(self, sock):
        status, size = struct.unpack('<iI', h.receive(sock, 8))
        return status, h.receive(sock, size)

    def test_every_c_encoded_operation_and_error_paths(self):
        (self.service.root / 'C/System/protected.txt').write_bytes(b'protected')
        (self.service.root / 'C/WINDOWS/TEMP/protocol-child.exe').write_bytes(b'native child')
        child = self.service.start_adapter(['installer-exec', 'protocol', 'protocol'], 'C protocol client')
        code, output = child.finish(15)
        self.assertEqual(code, 0, output + '\n' + child.session.diagnostic())
        self.assertFalse(self.service.failures)
        replies = [event for event in self.service.trace if event[0] == 'reply' and event[1] == child.session.pid]
        for op, actions in ((h.FILE, range(1, 22)), (h.OBJECT, range(1, 14)), (h.SYSTEM, range(1, 12))):
            actual = {int(event[2].split('action=')[1]) for event in replies if f' op=0x{op:x} ' in event[2]}
            self.assertEqual(actual, set(actions))
        for op in (h.PROCESS, h.WAIT):
            self.assertTrue(any(f' op=0x{op:x} ' in event[2] for event in replies))
        for op in range(1, 13):
            self.assertTrue(any(event[2].endswith(f' registry_op={op}') for event in replies))
        self.assertTrue(any('kind=3 size=0' in event[2] for event in replies))
        self.service.assert_quiescent(('protocol-child.exe',))
        print(output, end='')

    def test_malformed_complete_requests_reply_once_and_keep_alignment(self):
        with self.connection() as (session, sock):
            for raw in (b'short', wire(h.FILE, {4: 7, 6: 999999}),
                        wire(h.WAIT, {4: 999999, 5: 1})):
                with self.subTest(raw=raw.hex()):
                    status, out = self.request(sock, 1, raw)
                    self.assertIn(status, (-22, 0))
                    if status == 0:
                        self.assertEqual(struct.unpack_from('<I', out, 48)[0], 87)
                    else:
                        self.assertEqual(out, b'')
                    self.assertEqual(self.request(sock, 3), (0, session.cwd.encode() + b'\0'))
        self.assertEqual(len(self.service.failures), 1)
        self.assertIn('pointer offset outside message', str(self.service.failures))
        self.service.failures.clear()

    def test_handler_exception_and_oversized_reply_send_one_error(self):
        for response in (RuntimeError('injected handler failure'), (0, b'over capacity')):
            with self.subTest(response=response), self.connection() as (_, sock):
                with patch.object(h.Session, 'dispatch', side_effect=response if isinstance(response, Exception) else None,
                                  return_value=None if isinstance(response, Exception) else response):
                    self.assertEqual(self.request(sock, 3, cap=1),
                                     (-errno.EIO if isinstance(response, Exception) else -errno.EINVAL, b''))
                self.assertEqual(self.request(sock, 3)[0], 0)
        self.assertEqual(len(self.service.failures), 2)
        self.service.failures.clear()

    def test_receive_deadline_logs_partial_frame_replies_and_closes(self):
        self.service.request_timeout = 0.1
        header = struct.pack('<4I', h.MAGIC, 1, 72, h.LIMIT)
        for partial in (b'', header[:7], header, header + b'partial payload'):
            with self.subTest(partial=partial.hex()), self.connection() as (session, sock):
                h.send(sock, partial)
                self.assertEqual(self.reply(sock), (-errno.ETIMEDOUT, b''))
                self.assertIsNone(h.receive(sock, 1))
                self.assertEqual(bytes(session.pending_header), partial[:16])
                self.assertEqual(bytes(session.pending_payload), partial[16:])
            self.assertEqual(len(self.service.failures), 1)
            self.assertIn(partial[16:].hex(), self.service.failures[0][2])
            self.service.failures.clear()

    def test_receive_deadline_is_not_reset_by_partial_progress(self):
        class Poll:
            def register(self, fd, event): pass
            def poll(self, timeout): return [(0, 1)]
        class Socket:
            def fileno(self): return 0
        partial = bytearray()
        sock = Socket()
        with patch.object(h.time, 'monotonic', side_effect=[1, 2, 4]), \
             patch.object(h.select, 'poll', Poll), patch.object(h.os, 'read', return_value=b'x'):
            with self.assertRaises(TimeoutError): h.receive(sock, 16, deadline=3, partial=partial)
        self.assertEqual(partial, b'xx')

    def test_invalid_header_replies_once_and_closes(self):
        frames = (struct.pack('<4I', 0, 1, 0, 0),
                  struct.pack('<4I', h.MAGIC, 1, h.LIMIT + 1, 0))
        for frame in frames:
            with self.subTest(frame=frame.hex()), self.connection() as (_, sock):
                h.send(sock, frame)
                self.assertIn(self.reply(sock)[0], (-errno.EINVAL, -errno.EIO))
                self.assertIsNone(h.receive(sock, 1))
            self.assertEqual(len(self.service.failures), 1)
            self.service.failures.clear()

    def test_truncated_payload_attempts_one_error_reply(self):
        # Once the peer closes no reply can be delivered. Capture the single
        # attempted error response while exercising real partial reads/EOF.
        responses = []
        with patch.object(h, 'send', side_effect=lambda sock, raw: responses.append(raw)):
            with self.connection() as (session, sock):
                frame = struct.pack('<4I', h.MAGIC, 1, 72, h.LIMIT) + b'truncated'
                # Use descriptor I/O directly: send() is the observed server.
                import os
                self.assertEqual(os.write(sock.fileno(), frame), len(frame))
                sock.close()
        self.assertEqual(responses, [struct.pack('<iI', -errno.EIO, 0)])
        self.assertEqual(session.pending_payload, b'truncated')
        self.assertEqual(len(self.service.failures), 1)
        self.service.failures.clear()

    def test_finish_timeout_reports_last_operation(self):
        served = threading.Event()
        original = h.Session.dispatch
        def dispatch(session, kind, raw, cap):
            result = original(session, kind, raw, cap)
            served.set()
            return result
        with patch.object(h.Session, 'dispatch', dispatch):
            child = self.service.start_adapter(['installer-exec', 'protocol', 'protocol-hang'], 'timeout client')
            self.assertTrue(served.wait(2))
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                child.finish(0.1)
        self.assertIn('last served operation=kind=3 size=0', '\n'.join(caught.exception.__notes__))
        child.finish(2)
        self.assertFalse(child.worker.is_alive())


if __name__ == '__main__': unittest.main()
