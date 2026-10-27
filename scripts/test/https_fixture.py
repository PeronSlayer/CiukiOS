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
  /site/<name>/...  fx-accept: a local snapshot of a real site (see `load_sites`)

Site snapshots (fx-accept, the owner's two sites): a directory per host,
named after the host, with the saved `index.html` and the files saved beside
it, never committed (the owner's copies live in build/downloads/ns-sites-*).
The sorted directories are served as `/site/1/`, `/site/2/`, ... so no host
name enters a tracked file; `/site/<n>/` serves `index.html` and
`/site/<n>/<path>` that file. Absolute links to the site's own host
(`https://host/`, `http://host/`, `//host/`) and root-relative references in
HTML attributes and CSS `url()` are rewritten to
`https://fixture.test/site/<n>/` at serve time, so the browser fetches
everything the snapshot holds from the fixture; a path the snapshot does not
hold is aliased to the snapshot's only file with the same extension when there
is exactly one (a hashed asset name such as /build/assets/app-<hash>.css saved
as app.css), otherwise it is a recorded 404. Other hosts stay untouched and
fail in the guest (no route), which the acceptance case records.

TLS: the fixture certificate (tests/suites/fixtures/f6-browser/fixture.pem,
signed by the fixture CA) is presented for SNI `fixture.test`; every other
server name, including `untrusted.test`, gets the self-signed certificate, which
the browser must refuse. TLS 1.2 only, as the browser's BearSSL client offers.
"""
import argparse
import gzip
import http.server
import os
import posixpath
import re
import selectors
import socket
import ssl
import sys
import threading
from pathlib import Path
from urllib.parse import unquote

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evidence import EvidenceNotRun  # noqa: E402

GUEST_ADDRESS = '10.0.2.100'
FIXTURE_HOST = 'fixture.test'
UNTRUSTED_HOST = 'untrusted.test'
CERT_FILES = ('ca.pem', 'fixture.pem', 'fixture.key', 'untrusted.pem', 'untrusted.key')
GZIP_BYTES = 65536
PAGE = ('<!DOCTYPE html><html><head><title>Ciuki page</title></head>'
        '<body><h1>Ciuki page</h1><p>Served by the f6-net fixture over %s.</p></body></html>\n')


SITE_PREFIX = '/site/'
SITE_NAME_RE = re.compile(r'[a-z0-9][a-z0-9-]{0,15}')
SITE_HOST_RE = re.compile(r'[a-z0-9][a-z0-9.-]{0,252}')
SITE_SKIP = {'headers.txt', 'SHA256SUMS'}       # capture metadata, not site content
SITE_TYPES = {'.html': 'text/html; charset=utf-8', '.htm': 'text/html; charset=utf-8',
              '.css': 'text/css; charset=utf-8', '.js': 'application/javascript; charset=utf-8',
              '.json': 'application/json', '.webmanifest': 'application/manifest+json',
              '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
              '.gif': 'image/gif', '.ico': 'image/x-icon', '.webp': 'image/webp',
              '.woff': 'font/woff', '.woff2': 'font/woff2', '.txt': 'text/plain; charset=utf-8',
              '.xml': 'application/xml'}
SITE_REWRITE = ('.html', '.htm', '.css', '.js')
SITE_FILE_LIMIT = 8 * 1024 * 1024


class SitesAbsent(EvidenceNotRun):
    """The declared snapshots are not on this workstation: the case is not_run."""


def load_sites(declaration, root):
    """Resolve a case's `browser_sites` declaration into served snapshots.

    declaration: {"source": "<dir under the checkout>", "env": "<override
    variable>", "count": <n>}. The source directory (or $env) holds one
    sub-directory per host, named after it, with an index.html; the first n
    in sorted order are served as names "1".."n". Returns {name: {"host",
    "directory", "files"}}; raises SitesAbsent when the snapshots are not on
    this workstation (the case is then not_run, never pass) and ValueError
    for a malformed declaration."""
    if not isinstance(declaration, dict) or set(declaration) - {'source', 'env', 'count'}:
        raise ValueError('browser_sites declares only source, env and count')
    count = declaration.get('count')
    if type(count) is not int or not 1 <= count <= 4:
        raise ValueError('browser_sites.count is 1..4')
    env = declaration.get('env')
    if env is not None and not (isinstance(env, str) and re.fullmatch(r'[A-Z][A-Z0-9_]{0,31}', env)):
        raise ValueError('browser_sites.env is an environment variable name')
    source_name = declaration.get('source')
    if not isinstance(source_name, str) or not source_name or source_name.startswith('/') or '..' in source_name.split('/'):
        raise ValueError('browser_sites.source is a directory below the checkout')
    base = os.environ.get(env) if env else None
    source = Path(base) if base else Path(root) / source_name
    hosts = sorted(p.name for p in source.iterdir() if p.is_dir() and (p / 'index.html').is_file()) if source.is_dir() else []
    if len(hosts) < count:
        raise SitesAbsent('site snapshots absent: %d of %d under %s (set %s or place the snapshots there)'
                          % (len(hosts), count, source, env or 'browser_sites.source'))
    resolved = {}
    for number, host in enumerate(hosts[:count], 1):
        if not SITE_HOST_RE.fullmatch(host):
            raise ValueError('snapshot directory is not a host name: %r' % host)
        directory = (source / host).resolve()
        files = sorted(p.relative_to(directory).as_posix() for p in directory.rglob('*')
                       if p.is_file() and p.name not in SITE_SKIP)
        resolved[str(number)] = {'host': host, 'directory': directory, 'files': files}
    return resolved


def site_rewrite(data, host, name):
    """Point the snapshot's own absolute and root-relative references at the fixture."""
    base = ('https://%s%s%s/' % (FIXTURE_HOST, SITE_PREFIX, name)).encode()
    escaped = re.escape(host.encode())
    data = re.sub(rb'(?:https?:)?//' + escaped + rb'(?::[0-9]+)?/', base, data)
    data = re.sub(rb'(?:https?:)?//' + escaped + rb'(?=["\')\s])', base[:-1], data)
    data = re.sub(rb'((?:href|src|action|poster|data-src)\s*=\s*["\'])/(?!/)', rb'\1' + base, data, flags=re.I)
    data = re.sub(rb'(url\(\s*["\']?)/(?!/)', rb'\1' + base, data, flags=re.I)
    return data


def site_resolve(site, relative):
    """(file, alias) of a request path below /site/<name>/; (None, None) when absent."""
    relative = unquote(relative.split('?', 1)[0].split('#', 1)[0])
    if not relative or relative.endswith('/'):
        relative += 'index.html'
    normal = posixpath.normpath('/' + relative).lstrip('/')
    if normal.startswith('..') or '\x00' in normal:
        return None, None
    if normal in site['files']:
        return site['directory'] / normal, None
    extension = posixpath.splitext(normal)[1].lower()
    same = [f for f in site['files'] if posixpath.splitext(f)[1].lower() == extension]
    if extension in ('.css', '.js') and len(same) == 1:
        return site['directory'] / same[0], same[0]
    return None, None


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

    def _reply(self, status, body=b'', content_type='text/html; charset=utf-8', headers=(), extra=None):
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
                             'cookie': self.headers.get('Cookie'), 'accept_encoding': self.headers.get('Accept-Encoding'),
                             **(extra or {})})

    def do_HEAD(self):
        self.do_GET()

    def _site(self, path):
        """fx-accept: one file of a site snapshot, or a recorded 404."""
        name, _, relative = path[len(SITE_PREFIX):].partition('/')
        site = self.server.sites.get(name)
        if site is None:
            self._reply(404, b'<p>no such site snapshot</p>\n', extra={'site': name, 'missing': True})
            return
        if not relative and not path.endswith('/'):
            self._reply(301, b'', headers=[('Location', SITE_PREFIX + name + '/')], extra={'site': name})
            return
        target, alias = site_resolve(site, relative)
        if target is None or not target.is_file() or target.stat().st_size > SITE_FILE_LIMIT:
            self._reply(404, b'<p>not in the snapshot</p>\n', extra={'site': name, 'missing': True})
            return
        body = target.read_bytes()
        extension = target.suffix.lower()
        if extension in SITE_REWRITE:
            body = site_rewrite(body, site['host'], name)
        self._reply(200, body, content_type=SITE_TYPES.get(extension, 'application/octet-stream'),
                    extra={'site': name, 'file': target.relative_to(site['directory']).as_posix(),
                           'alias': alias, 'bytes': len(body)})

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        secure = self.server.scheme == 'https'
        if path.startswith(SITE_PREFIX) and self.server.sites:
            self._site(path)
        elif path == '/page':
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

    def __init__(self, scheme, observe, gzip_compressed, sites=None):
        self.scheme = scheme
        self.observe = observe
        self.gzip_compressed = gzip_compressed
        self.sites = sites or {}
        super().__init__(('127.0.0.1', 0), Handler)


class _TLSServer(_Server):
    def __init__(self, observe, gzip_compressed, certs, sites=None):
        super().__init__('https', observe, gzip_compressed, sites)
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
    def __init__(self, certs, sites=None):
        self.certs = Path(certs)
        missing = [name for name in CERT_FILES if not (self.certs / name).is_file()]
        if missing:
            raise ValueError('browser fixture certificates missing: ' + ', '.join(missing))
        self.sites = sites or {}
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
        self._http = _Server('http', self.observe, self.gzip_compressed, self.sites)
        self._https = _TLSServer(self.observe, self.gzip_compressed, self.certs, self.sites)
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
            out = {'http_port': self.http_port, 'https_port': self.https_port, 'requests': list(self.requests)}
        if self.sites:
            # Host names stay out of the evidence: the served name and the file count only.
            out['sites'] = {name: {'files': len(s['files'])} for name, s in self.sites.items()}
        return out

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
