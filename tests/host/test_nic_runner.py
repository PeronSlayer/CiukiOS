"""F6 selectors, declared NIC arguments and strict ARP evidence; no QEMU."""
import copy
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from loader_model import selector
from evidence import Parser, EvidenceError
import run as runner
import resources as res


class NicRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = json.loads((ROOT / 'tests/suites/f6-net.json').read_text())['cases']
        out = ROOT / 'build/host'
        out.mkdir(parents=True, exist_ok=True)
        shim = out / 'nic-selector.c'
        shim.write_text(r'''
#include <ciuki/kernel.h>
#include "selector.h"
static unsigned calls;
static int arp(void) { calls++; return 0; }
void rec_emit(const char *p, const char *e, const char *f, ...) { (void)p;(void)e;(void)f; }
void klog(const char *f, ...) { (void)f; }
int validate(const char *s, unsigned n, unsigned flags) {
    struct probe_selection selection;
    return probes_parse_selector(s,n,flags,&selection) ? selection.phase+1 : 0;
}
unsigned dispatch(void) {
    const struct probe_def table[]={{"net-arp",arp}};
    struct probe_tables tables={.f6=table,.f6_count=1};
    const struct probe_selection selection={.phase=6,.probe="net-arp"};
    struct probe_hooks hooks={0}; calls=0;
    probes_dispatch(&selection,&tables,&hooks); return calls;
}
''')
        lib = out / 'nic-selector.so'
        subprocess.run(['nice', '-n', '19', 'clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I', str(ROOT / 'src/kernel/include'),
                        '-I', str(ROOT / 'src/kernel/probes'), str(shim),
                        str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(lib)], check=True)
        cls.native = ctypes.CDLL(str(lib))
        cls.native.validate.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint]

    def test_selectors_and_production_dispatch(self):
        request = 'f6:net-arp run=12345678'
        self.assertEqual(selector(request)['phase'], 6)
        self.assertEqual(self.native.validate(request.encode(), len(request), 0), 7)
        self.assertEqual(self.native.dispatch(), 1)
        for bad in ('f5:net-arp run=12345678', 'f6:all run=12345678',
                    'f6:sweep run=12345678', 'f6:net-arp run=1234567',
                    'f6:net-arp run=12345678 extra=x'):
            with self.subTest(request=bad):
                with self.assertRaises(ValueError):
                    selector(bad)
                self.assertEqual(self.native.validate(bad.encode(), len(bad), 0), 0)
        # E500 suffix remains provenance-checked by both production parsers.
        forced = request + ' platform=e500'
        with self.assertRaises(ValueError):
            selector(forced)
        self.assertEqual(selector(forced, 'fw_cfg', True)['platform'], 'e500')
        self.assertEqual(self.native.validate(forced.encode(), len(forced), 0), 0)

    def test_twin_matrix_and_explicit_network(self):
        self.assertEqual(len(self.cases), 24)
        pairs = {(c['profile'], c['device_exceptions']['network']) for c in self.cases}
        self.assertEqual(len(pairs), 24)
        self.assertEqual({nic for _, nic in pairs}, {'rtl8139', 'ne2k_pci', 'i82559er', 'e1000', 'pcnet', 'tulip'})
        for case in self.cases:
            profile = json.loads((ROOT / 'tests/profiles' / (case['profile'] + '.json')).read_text())
            args, request = runner.qemu_args('qemu-fixture', profile, case, '12345678', Path('overlay.qcow2'), Path('bios'))
            self.assertIn('-nic', args)
            self.assertIn('user,id=ciuknet0,restrict=on', args)
            self.assertIn(case['device_exceptions']['network'] + ',netdev=ciuknet0,mac=52:54:00:12:34:56,romfile=', args)
            self.assertTrue(request.startswith('f6:net-arp run=12345678'))
            self.assertEqual('platform=e500' in request, case['profile'] == 'qemu-e500')
        profile = json.loads((ROOT / 'tests/profiles/qemu-t23.json').read_text())
        for nic in ('e1000e', '3c905', 'rhine', '', 'rtl8139,hostfwd=tcp::22-:22'):
            bad = copy.deepcopy(self.cases[0]); bad['device_exceptions']['network'] = nic
            with self.assertRaises(res.Refusal):
                runner.qemu_args('fixture', profile, bad, '12345678', Path('overlay'), Path('bios'))
        bad = copy.deepcopy(self.cases[0]); bad.update(probe='boot', selector='f0:boot run={run_id}')
        with self.assertRaises(res.Refusal):
            runner.qemu_args('fixture', profile, bad, '12345678', Path('overlay'), Path('bios'))

    def test_arp_predicates_reject_incomplete_and_false_success(self):
        records = [dict(event='BEGIN'),
                   dict(event='READY', driver='rtl8139', mac='52:54:00:12:34:56', mode='intx'),
                   dict(event='DATA', case='arp', arp='ok', mac='52:55:0a:00:02:02', tx=1, rx=1, errors=0, dropped=0),
                   dict(event='DATA', case='budget', max_irq_us=10, violations=0, interrupts=1, quarantine=0),
                   dict(event='DATA', case='cleanup', stopped=1, dma_pages=0, irq_attached=0),
                   dict(event='END', status='PASS')]
        def parse(rows):
            parser = Parser('12345678', 'net-arp')
            for seq, row in enumerate(rows, 1):
                parser.feed(('CIUKI_TEST v=1 run=12345678 seq=%06d probe=net-arp ' % seq +
                             ' '.join(f'{k}={v}' for k, v in row.items())).encode())
            parser.check(self.cases[0]['expected'])
        parse(records)
        for index, key, value in ((2, 'arp', 'fail'), (2, 'mac', '00:00:00:00:00:00'),
                                  (2, 'tx', 0), (2, 'rx', 0), (2, 'errors', 1),
                                  (3, 'max_irq_us', 51), (3, 'quarantine', 1), (3, 'interrupts', 0),
                                  (4, 'stopped', 0), (4, 'dma_pages', 5), (4, 'irq_attached', 1)):
            broken = copy.deepcopy(records); broken[index][key] = value
            with self.subTest(key=key), self.assertRaises(EvidenceError):
                parse(broken)
        with self.assertRaises(EvidenceError):
            parse(records[:2] + records[3:])

    def test_second_set_requires_identity_carrier_and_complete_evidence(self):
        for case in self.cases:
            name = case['device_exceptions']['network']
            if name not in ('e1000', 'pcnet', 'tulip'):
                continue
            def check(driver, link):
                parser = Parser('12345678', 'net-arp')
                rows = [f'event=BEGIN',
                        f'event=READY driver={driver} mac=52:54:00:12:34:56 mode=intx link={link}',
                        'event=DATA case=arp arp=ok mac=52:55:0a:00:02:02 tx=1 rx=1 errors=0 dropped=0',
                        'event=DATA case=budget max_irq_us=10 violations=0 interrupts=1 quarantine=0',
                        'event=DATA case=cleanup stopped=1 dma_pages=0 irq_attached=0',
                        'event=END status=PASS']
                for seq, row in enumerate(rows, 1):
                    parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=net-arp {row}'.encode())
                parser.check(case['expected'])
            check(name, 'up')
            with self.assertRaises(EvidenceError):
                check(name, 'down')
            with self.assertRaises(EvidenceError):
                check('rtl8139', 'up')
