#!/usr/bin/env python3
"""f6-18: host HTTP/HTTPS fixture for the f6-net browser cases. Standard library only.

The runner starts one FixtureServer per browser case on 127.0.0.1 (ephemeral
ports) and reaches it from the guest through QEMU's user network:
`-netdev user,...,guestfwd=tcp:10.0.2.100:443-cmd:<relay>` spawns the relay
mode of this script for every guest connection (libslirp runs the command
through /bin/sh -c and pipes the connection on stdin/stdout). The guest
resolves `fixture.test` and `untrusted.test` to 10.0.2.100 through the kernel's
test-profile resolver entry (src/kernel/net/lwip/sockets.c, armed only by the
`f6:browser` selector), so no DNS and no hosts file take part.

Routes (docs/validation/f6-18-browser-https-cases.md maps them to the f6-17
host-test routes):

  /page        200 text/html, the "Ciuki page"
  /upgrade     302 to https://fixture.test/cookie (the http -> https redirect)
  /cookie      HTTPS only: Set-Cookie ciuki=1; Secure; HttpOnly, then 302 to /cookie-end
  /cookie-end  200 "cookie ok" when the Secure cookie came back, else 403
  /gzip64      200 text/plain, Content-Encoding gzip, exactly 65,536 decoded bytes

TLS: the fixture certificate (tests/suites/fixtures/f6-browser/fixture.pem,
signed by the fixture CA) is presented for SNI `fixture.test`; every other
server name, including `untrusted.test`, gets the self-signed certificate, which
the browser must refuse. TLS 1.2 only, as the browser's BearSSL client offers.
"""
import argparse
import gzip
import http.server
import os
import selectors
import socket
import ssl
import sys
import threading
from pathlib import Path

GUEST_ADDRESS = '10.0.2.100'
FIXTURE_HOST = 'fixture.test'
UNTRUSTED_HOST = 'untrusted.test'
CERT_FILES = ('ca.pem', 'fixture.pem', 'fixture.key', 'untrusted.pem', 'untrusted.key')
GZIP_BYTES = 65536
PAGE = ('<!DOCTYPE html><html><head><title>Ciuki page</title></head>'
        '<body><h1>Ciuki page</h1><p>Served by the f6-net fixture over %s.</p></body></html>\n')


def gzip_body():
    line = b'0123456789abcdef' * 3 + b'ciuki-gzip-64k\n'   # 63 bytes
    raw = (line * (GZIP_BYTES // len(line) + 1))[:GZIP_BYTES]
    assert len(raw) == GZIP_BYTES
    return raw, gzip.compress(raw, compresslevel=6, mtime=0)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'CiukiFixture/1'
    sys_version = ''

    def log_message(self, format, *args):  # noqa: A002 - BaseHTTPRequestHandler's signature
        return

    def _sni(self):
        return getattr(self.connection, 'ciuki_sni', None)

    def _reply(self, status, body=b'', content_type='text/html; charset=utf-8', headers=()):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)
        self.server.observe({'scheme': self.server.scheme, 'method': self.command, 'path': self.path,
                             'host': self.headers.get('Host'), 'sni': self._sni(), 'status': status,
                             'cookie': self.headers.get('Cookie'), 'accept_encoding': self.headers.get('Accept-Encoding')})

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        secure = self.server.scheme == 'https'
        if path == '/page':
            self._reply(200, (PAGE % self.server.scheme).encode())
        elif path == '/upgrade':
            self._reply(302, b'', headers=[('Location', 'https://%s/cookie' % FIXTURE_HOST)])
        elif path == '/cookie':
            if not secure:
                self._reply(403, b'<p>cookies only over TLS</p>\n')
            else:
                self._reply(302, b'', headers=[('Set-Cookie', 'ciuki=1; Path=/; Secure; HttpOnly'),
                                               ('Location', '/cookie-end')])
        elif path == '/cookie-end':
            cookies = [c.strip() for c in (self.headers.get('Cookie') or '').split(';')]
            if secure and 'ciuki=1' in cookies:
                self._reply(200, b'<!DOCTYPE html><html><head><title>cookie ok</title></head><body><p>cookie ok</p></body></html>\n')
            else:
                self._reply(403, b'<p>cookie missing</p>\n')
        elif path == '/gzip64':
            self._reply(200, self.server.gzip_compressed, content_type='text/plain; charset=utf-8',
                        headers=[('Content-Encoding', 'gzip')])
        else:
            self._reply(404, b'<p>no such fixture route</p>\n')


class _Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, scheme, observe, gzip_compressed):
        self.scheme = scheme
        self.observe = observe
        self.gzip_compressed = gzip_compressed
        super().__init__(('127.0.0.1', 0), Handler)


class _TLSServer(_Server):
    def __init__(self, observe, gzip_compressed, certs):
        super().__init__('https', observe, gzip_compressed)
        self.fixture_context = self._context(certs / 'fixture.pem', certs / 'fixture.key')
        self.untrusted_context = self._context(certs / 'untrusted.pem', certs / 'untrusted.key')
        self.fixture_context.sni_callback = self._sni

    @staticmethod
    def _context(certificate, key):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(certificate), str(key))
        return context

    def _sni(self, sslsocket, server_name, context):
        sslsocket.ciuki_sni = server_name
        if server_name != FIXTURE_HOST:
            sslsocket.context = self.untrusted_context

    def get_request(self):
        connection, address = super().get_request()
        try:
            wrapped = self.fixture_context.wrap_socket(connection, server_side=True)
        except (ssl.SSLError, OSError) as error:
            self.observe({'scheme': 'https', 'handshake_error': type(error).__name__, 'detail': str(error)[:120]})
            connection.close()
            raise OSError('TLS handshake refused') from error
        return wrapped, address


class FixtureServer:
    """Both listeners on 127.0.0.1, ephemeral ports, served by daemon threads."""
    def __init__(self, certs):
        self.certs = Path(certs)
        missing = [name for name in CERT_FILES if not (self.certs / name).is_file()]
        if missing:
            raise ValueError('browser fixture certificates missing: ' + ', '.join(missing))
        self.requests = []
        self._lock = threading.Lock()
        self.gzip_raw, self.gzip_compressed = gzip_body()
        self._http = self._https = None
        self._threads = []

    def observe(self, entry):
        with self._lock:
            if len(self.requests) < 512:
                self.requests.append(entry)

    @property
    def http_port(self):
        return self._http.server_address[1]

    @property
    def https_port(self):
        return self._https.server_address[1]

    def start(self):
        self._http = _Server('http', self.observe, self.gzip_compressed)
        self._https = _TLSServer(self.observe, self.gzip_compressed, self.certs)
        for server in (self._http, self._https):
            thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.1},
                                      name='ciuki-fixture-' + server.scheme, daemon=True)
            thread.start()
            self._threads.append(thread)
        return self

    def stop(self):
        for server in (self._http, self._https):
            if server is None:
                continue
            server.shutdown()
            server.server_close()
        for thread in self._threads:
            thread.join(timeout=5)
        self._threads = []

    def snapshot(self):
        with self._lock:
            return {'http_port': self.http_port, 'https_port': self.https_port, 'requests': list(self.requests)}

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def relay_command(port, executable=None, script=None):
    """The guestfwd `cmd:` string for one listener: no commas, QEMU splits on them."""
    executable = executable or sys.executable
    script = str(script or Path(__file__).resolve())
    for part in (executable, script):
        if ',' in part or not part.startswith('/'):
            raise ValueError('relay path must be absolute and free of commas: ' + part)
    return '%s %s relay 127.0.0.1 %d' % (executable, script, port)


def guestfwd_options(http_port, https_port, executable=None, script=None):
    """`guestfwd=` option strings for the runner's -netdev user,...; order http then https."""
    return ['guestfwd=tcp:%s:80-cmd:%s' % (GUEST_ADDRESS, relay_command(http_port, executable, script)),
            'guestfwd=tcp:%s:443-cmd:%s' % (GUEST_ADDRESS, relay_command(https_port, executable, script))]


def relay(host, port):
    """stdin/stdout <-> TCP pump for libslirp's exec forwarding; exits on both EOFs."""
    upstream = socket.create_connection((host, port), timeout=10)
    upstream.settimeout(None)
    os.set_blocking(0, False)
    pump = selectors.DefaultSelector()
    pump.register(0, selectors.EVENT_READ, 'guest')
    pump.register(upstream, selectors.EVENT_READ, 'server')
    open_sides = 2
    idle_rounds = 0
    while open_sides and idle_rounds < 4:      # four quiet minutes end a forgotten pump
        ready = pump.select(timeout=60)
        idle_rounds = idle_rounds + 1 if not ready else 0
        for key, _ in ready:
            if key.data == 'guest':
                try:
                    chunk = os.read(0, 65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    pump.unregister(0); open_sides -= 1
                    try:
                        upstream.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                    continue
                upstream.sendall(chunk)
            else:
                chunk = upstream.recv(65536)
                if not chunk:
                    pump.unregister(upstream); open_sides -= 1
                    try:   # libslirp hands us a socketpair: half-close toward the guest
                        guest = socket.socket(fileno=os.dup(1))
                        guest.shutdown(socket.SHUT_WR); guest.close()
                    except OSError:
                        return 0   # a pipe cannot half-close; exiting closes it
                    continue
                view = memoryview(chunk)
                while view:
                    try:
                        written = os.write(1, view)
                    except BlockingIOError:
                        continue
                    except OSError:
                        return 0
                    view = view[written:]
    upstream.close()
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='mode', required=True)
    r = sub.add_parser('relay', help='stdin/stdout to TCP relay (guestfwd cmd:)')
    r.add_argument('host'); r.add_argument('port', type=int)
    s = sub.add_parser('serve', help='serve the fixture routes until interrupted (manual checks)')
    s.add_argument('--certs', default=str(Path(__file__).resolve().parents[2] / 'tests/suites/fixtures/f6-browser'))
    args = parser.parse_args(argv)
    if args.mode == 'relay':
        return relay(args.host, args.port)
    with FixtureServer(args.certs) as fixture:
        print('http=%d https=%d' % (fixture.http_port, fixture.https_port), flush=True)
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
