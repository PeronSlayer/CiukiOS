"""f6-18: the f6:browser selector (model, runner, kernel), the guestfwd QEMU
arguments, the host HTTPS fixture and the suite's browser cases; no QEMU."""
import copy
import ctypes
import gzip
import http.client
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from loader_model import selector  # noqa: E402
from evidence import Parser, EvidenceError  # noqa: E402
import https_fixture  # noqa: E402
import probe_tables  # noqa: E402
import run as runner  # noqa: E402
import resources as res  # noqa: E402

CERTS = ROOT / 'tests/suites/fixtures/f6-browser'
GOOD = 'f6:browser run=12345678 url=https://fixture.test/page'


class BrowserSelectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        shim = out / 'browser-selector.c'
        shim.write_text(r'''
#include <ciuki/kernel.h>
#include "selector.h"
void rec_emit(const char *p, const char *e, const char *f, ...) { (void)p;(void)e;(void)f; }
void klog(const char *f, ...) { (void)f; }
/* 0: refused; otherwise 1 + flags bit (now=1) and the URL copied out. */
int validate(const char *s, unsigned n, unsigned flags, char *url, unsigned cap) {
    struct probe_selection selection;
    if (!probes_parse_selector(s,n,flags,&selection)) return 0;
    unsigned i = 0; while (selection.url[i] && i + 1 < cap) { url[i] = selection.url[i]; i++; } url[i] = 0;
    return 1 + ((selection.flags & PROBE_BROWSER_NOW) ? 2 : 0) + (selection.phase == 6 ? 4 : 0);
}
''')
        lib = out / 'browser-selector.so'
        subprocess.run(['nice', '-n', '19', 'clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I', str(ROOT / 'src/kernel/include'),
                        '-I', str(ROOT / 'src/kernel/probes'), str(shim),
                        str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(lib)], check=True)
        cls.native = ctypes.CDLL(str(lib))
        cls.native.validate.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_char_p, ctypes.c_uint]

    def kernel(self, request):
        url = ctypes.create_string_buffer(64)
        code = self.native.validate(request.encode(), len(request), 0, url, 64)
        return code, url.value.decode()

    def test_model_runner_and_kernel_accept_the_same_requests(self):
        for request, url, now in ((GOOD, 'https://fixture.test/page', False),
                                  ('f6:browser run=0000abcd url=http://fixture.test/upgrade', 'http://fixture.test/upgrade', False),
                                  ('f6:browser run=12345678 url=https://fixture.test/page now=1', 'https://fixture.test/page', True),
                                  ('f6:browser run=12345678 url=https://h:8443/a_b-c.d/', 'https://h:8443/a_b-c.d/', False)):
            with self.subTest(request=request):
                self.assertLessEqual(len(request), 64)
                for parsed in (selector(request), runner.selector(request, 'fw_cfg', True)):
                    self.assertEqual((parsed['phase'], parsed['probe'], parsed['url'], parsed['now']), (6, 'browser', url, now))
                    self.assertIsNone(parsed['platform']); self.assertFalse(parsed['safe'])
                self.assertEqual(self.kernel(request), (1 + 4 + (2 if now else 0), url))

    def test_rejected_requests_are_rejected_by_both_parsers(self):
        bad = ['f6:browser run=12345678',                                  # URL mandatory
               'f6:browser run=12345678 url=ftp://fixture.test/page',      # scheme
               'f6:browser run=12345678 url=https://fixture.test',         # path mandatory
               'f6:browser run=12345678 url=https://fixture.test/p?q=1',   # no query
               'f6:browser run=12345678 url=https://fixture.test/a=b',     # record grammar
               'f6:browser run=12345678 url=https://fixture.test/page now=0',
               'f6:browser run=12345678 url=https://fixture.test/page now=1 now=1',
               'f6:browser run=12345678 url=https://fixture.test/page platform=e500',
               'f6:browser run=12345678 url=https://fixture.test/page safe=1',
               'f6:browser run=1234567 url=https://fixture.test/page',
               'f6:browser run=12345678 url=https://h/' + 'a' * 27,   # 65 bytes
               'f5:browser run=12345678 url=https://fixture.test/page',
               'f6:browser run=12345678  url=https://fixture.test/page']
        for request in bad:
            with self.subTest(request=request):
                with self.assertRaises(ValueError):
                    selector(request)
                with self.assertRaises(ValueError):
                    runner.selector(request, 'fw_cfg', True)
                self.assertEqual(self.kernel(request)[0], 0)
        longest = 'f6:browser run=12345678 url=https://fixture.test/' + 'a' * 9 + ' now=1'
        self.assertEqual(len(longest), 64)
        self.assertEqual(self.kernel(longest)[0], 7)
        self.assertEqual(selector(longest)['now'], True)

    def test_kernel_menu_and_model_tables_know_the_probe(self):
        # The loader menu registers f6:browser after f0-54 (assembled grammar:
        # test_boot_modes.LoaderParserTests).
        self.assertEqual(probe_tables.registration(6, 'browser'),
                         {'kernel': True, 'menu': True, 'loader_model': True})
        self.assertEqual(probe_tables.registration(6, 'net-arp'),
                         {'kernel': True, 'menu': True, 'loader_model': True})


class BrowserQemuArgumentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = [c for c in json.loads((ROOT / 'tests/suites/f6-net.json').read_text())['cases'] if c['probe'] == 'browser']
        cls.profile = json.loads((ROOT / 'tests/profiles/qemu-t23.json').read_text())

    def test_browser_cases_use_guestfwd_to_the_fixture_ports(self):
        self.assertEqual([c['id'] for c in self.cases],
                         ['https-trusted', 'https-untrusted', 'http-redirect-https', 'https-cookies', 'https-gzip-64k', 'https-not-ready'])
        for case in self.cases:
            with self.subTest(case=case['id']):
                self.assertEqual(case['profile'], 'qemu-t23')
                self.assertEqual(case['device_exceptions'], {'network': 'e1000'})
                self.assertTrue(runner.browser_case(case))
                staged = {**case, '_browser_ports': (40080, 40443)}
                args, request = runner.qemu_args('qemu-fixture', self.profile, staged, '12345678', Path('overlay.qcow2'), Path('bios'))
                self.assertEqual(request, case['selector'].format(run_id='12345678'))
                netdev = args[args.index('-netdev') + 1]
                self.assertTrue(netdev.startswith('user,id=ciuknet0,restrict=on,guestfwd=tcp:10.0.2.100:80-cmd:'))
                self.assertIn(',guestfwd=tcp:10.0.2.100:443-cmd:', netdev)
                self.assertIn(' relay 127.0.0.1 40080', netdev)
                self.assertIn(' relay 127.0.0.1 40443', netdev)
                self.assertEqual(netdev.count(','), 4, 'the relay command must not contain commas')
                self.assertIn('e1000,netdev=ciuknet0,mac=52:54:00:12:34:56,romfile=', args)
                self.assertIn('-fw_cfg', args)
                self.assertIn('name=opt/it.alcybercloud.ciukios/test,string=' + request, args)

    def test_refusals(self):
        case = copy.deepcopy(self.cases[0])
        with self.assertRaises(res.Refusal):   # fixture ports come from _run_boot
            runner.qemu_args('fixture', self.profile, case, '12345678', Path('overlay'), Path('bios'))
        without = copy.deepcopy(case); without.pop('device_exceptions'); without['_browser_ports'] = (1, 2)
        with self.assertRaises(res.Refusal):   # a browser case needs the user network
            runner.qemu_args('fixture', self.profile, without, '12345678', Path('overlay'), Path('bios'))
        other = copy.deepcopy(case); other.update(probe='boot', selector='f0:boot run={run_id}', _browser_ports=(1, 2))
        with self.assertRaises(res.Refusal):   # the user network stays F6-only
            runner.qemu_args('fixture', self.profile, other, '12345678', Path('overlay'), Path('bios'))
        for nic in ('rtl8139,hostfwd=tcp::22-:22', 'e1000e'):
            bad = copy.deepcopy(case); bad['device_exceptions']['network'] = nic; bad['_browser_ports'] = (1, 2)
            with self.assertRaises(res.Refusal):
                runner.qemu_args('fixture', self.profile, bad, '12345678', Path('overlay'), Path('bios'))
        e500 = json.loads((ROOT / 'tests/profiles/qemu-e500.json').read_text())
        with self.assertRaises(res.Refusal):   # no platform suffix in the browser grammar
            runner.qemu_args('fixture', e500, {**case, '_browser_ports': (1, 2)}, '12345678', Path('overlay'), Path('bios'))

    def test_page_predicates_reject_wrong_outcomes(self):
        by_id = {c['id']: c for c in self.cases}
        def parse(case, page, waited=1, ready=1, now=0):
            rows = ['event=BEGIN',
                    'event=DATA case=write-gate expected=0 observed=0',
                    'event=DATA case=network primary=0 address=10.0.2.15 dhcp_error=0 wait_ms=1200',
                    'event=DATA case=resolver names=fixture.test,untrusted.test address=10.0.2.100 armed=0',
                    f'event=DATA case=entropy waited={waited} ready={ready} polls=3 wait_ms=700',
                    f'event=DATA case=fixture path=/Programs/netsurf pid=9 server_pid=5 url=x now={now}',
                    'event=DATA case=page ' + page,
                    'event=DATA case=ledger objects_equal=1 processes_equal=1',
                    'event=END status=PASS']
            parser = Parser('12345678', 'browser')
            for seq, row in enumerate(rows, 1):
                parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=browser {row}'.encode())
            parser.check(case['expected'])
        parse(by_id['https-trusted'], 'status=200 bytes=150 tls=ok url=https://fixture.test/page')
        parse(by_id['https-untrusted'], 'status=0 bytes=0 tls=cert url=about:query/fetcherror')
        parse(by_id['http-redirect-https'], 'status=200 bytes=90 tls=ok url=https://fixture.test/cookie-end')
        parse(by_id['https-cookies'], 'status=200 bytes=90 tls=ok url=https://fixture.test/cookie-end')
        parse(by_id['https-gzip-64k'], 'status=200 bytes=65536 tls=ok url=https://fixture.test/gzip64')
        parse(by_id['https-not-ready'], 'status=0 bytes=0 tls=not_ready url=about:query/fetcherror', waited=0, ready=0, now=1)
        wrong = [('https-trusted', 'status=200 bytes=150 tls=cert url=https://fixture.test/page'),
                 ('https-trusted', 'status=200 bytes=150 tls=ok url=http://fixture.test/page'),
                 ('https-untrusted', 'status=200 bytes=150 tls=ok url=https://untrusted.test/page'),
                 ('http-redirect-https', 'status=403 bytes=20 tls=ok url=https://fixture.test/cookie-end'),
                 ('https-cookies', 'status=200 bytes=90 tls=ok url=https://fixture.test/cookie'),
                 ('https-gzip-64k', 'status=200 bytes=65535 tls=ok url=https://fixture.test/gzip64'),
                 ('https-not-ready', 'status=200 bytes=150 tls=ok url=https://fixture.test/page')]
        for case_id, page in wrong:
            with self.subTest(case=case_id, page=page), self.assertRaises(EvidenceError):
                parse(by_id[case_id], page, **({'waited': 0, 'ready': 0, 'now': 1} if case_id == 'https-not-ready' else {}))
        with self.assertRaises(EvidenceError):   # a ready case that did not wait for the pool
            parse(by_id['https-trusted'], 'status=200 bytes=150 tls=ok url=https://fixture.test/page', waited=0, ready=0)


class FixtureServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = https_fixture.FixtureServer(CERTS).start()
        cls.trusting = ssl.create_default_context(cafile=str(CERTS / 'ca.pem'))
        cls.trusting.minimum_version = ssl.TLSVersion.TLSv1_2

    @classmethod
    def tearDownClass(cls):
        cls.fixture.stop()

    def https(self, host, path, context=None, headers=None):
        context = context or self.trusting
        raw = socket.create_connection(('127.0.0.1', self.fixture.https_port), timeout=5)
        tls = context.wrap_socket(raw, server_hostname=host)
        try:
            connection = http.client.HTTPConnection(host)
            connection.sock = tls
            connection.request('GET', path, headers={'Host': host, **(headers or {})})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read(), tls.version()
        finally:
            tls.close()

    def http(self, path, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.fixture.http_port, timeout=5)
        try:
            connection.request('GET', path, headers={'Host': 'fixture.test', **(headers or {})})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_trusted_page_over_tls12_with_the_fixture_ca_only(self):
        status, headers, body, version = self.https('fixture.test', '/page')
        self.assertEqual((status, version), (200, 'TLSv1.2'))
        self.assertIn(b'<title>Ciuki page</title>', body)
        self.assertEqual(int(headers['Content-Length']), len(body))
        production = ssl.create_default_context()
        with self.assertRaises(ssl.SSLCertVerificationError):   # the fixture CA is not a public root
            self.https('fixture.test', '/page', production)

    def test_untrusted_sni_gets_the_self_signed_certificate(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.https('untrusted.test', '/page')
        permissive = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        permissive.check_hostname = False; permissive.verify_mode = ssl.CERT_NONE
        raw = socket.create_connection(('127.0.0.1', self.fixture.https_port), timeout=5)
        with permissive.wrap_socket(raw, server_hostname='untrusted.test') as tls:
            der = tls.getpeercert(binary_form=True)
        self.assertEqual(der, ssl.PEM_cert_to_DER_cert((CERTS / 'untrusted.pem').read_text()))

    def test_redirect_and_secure_cookie_round_trip(self):
        status, headers, _ = self.http('/upgrade')
        self.assertEqual((status, headers['Location']), (302, 'https://fixture.test/cookie'))
        status, headers, _, _ = self.https('fixture.test', '/cookie')
        self.assertEqual((status, headers['Location']), (302, '/cookie-end'))
        self.assertEqual(headers['Set-Cookie'], 'ciuki=1; Path=/; Secure; HttpOnly')
        self.assertEqual(self.https('fixture.test', '/cookie-end', headers={'Cookie': 'ciuki=1'})[0], 200)
        self.assertEqual(self.https('fixture.test', '/cookie-end')[0], 403)
        self.assertEqual(self.http('/cookie')[0], 403)        # never over plain HTTP
        self.assertEqual(self.http('/cookie-end', headers={'Cookie': 'ciuki=1'})[0], 403)

    def test_gzip_body_decodes_to_exactly_64_kib(self):
        status, headers, body, _ = self.https('fixture.test', '/gzip64', headers={'Accept-Encoding': 'gzip'})
        self.assertEqual((status, headers['Content-Encoding']), (200, 'gzip'))
        self.assertEqual(len(gzip.decompress(body)), 65536)
        self.assertEqual(self.https('fixture.test', '/missing')[0], 404)
        snapshot = self.fixture.snapshot()
        self.assertEqual({snapshot['http_port'], snapshot['https_port']}, {self.fixture.http_port, self.fixture.https_port})
        self.assertTrue(any(r.get('path') == '/gzip64' and r.get('sni') == 'fixture.test' for r in snapshot['requests']))

    def test_guestfwd_options_and_relay_command(self):
        options = https_fixture.guestfwd_options(1080, 1443, '/usr/bin/python3', '/repo/scripts/test/https_fixture.py')
        self.assertEqual(options, ['guestfwd=tcp:10.0.2.100:80-cmd:/usr/bin/python3 /repo/scripts/test/https_fixture.py relay 127.0.0.1 1080',
                                   'guestfwd=tcp:10.0.2.100:443-cmd:/usr/bin/python3 /repo/scripts/test/https_fixture.py relay 127.0.0.1 1443'])
        with self.assertRaises(ValueError):
            https_fixture.relay_command(1, '/usr/bin/python3', '/a,b/relay.py')
        with self.assertRaises(ValueError):
            https_fixture.relay_command(1, 'python3', '/repo/relay.py')

    def test_relay_pumps_a_request_through_stdio(self):
        # The relay as libslirp runs it: a socketpair on stdin/stdout, TCP upstream.
        guest, child = socket.socketpair()
        process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/test/https_fixture.py'), 'relay', '127.0.0.1', str(self.fixture.http_port)],
                                   stdin=child, stdout=child, stderr=subprocess.PIPE)
        child.close()
        try:
            guest.sendall(b'GET /page HTTP/1.1\r\nHost: fixture.test\r\nConnection: close\r\n\r\n')
            guest.settimeout(10)
            data = b''
            while True:
                chunk = guest.recv(65536)
                if not chunk:
                    break
                data += chunk
            self.assertTrue(data.startswith(b'HTTP/1.1 200 OK'))
            self.assertIn(b'Ciuki page', data)
            guest.close()
            self.assertEqual(process.wait(timeout=10), 0)
        finally:
            if process.poll() is None:
                process.kill()
            process.stderr.close()


if __name__ == '__main__':
    unittest.main()
