"""f3-61b: the f3:vm-game run's records against the runner's evidence rules.

QEMU run cade4804 (image 6b586080) failed with "version, run or probe
mismatch": the DOS window path tagged its admission record probe=vm-window
inside the vm-game run. A run carries exactly one probe name
(scripts/test/evidence.py); the selected controller owns the window path's
records (dosvm_client.c window_probe, as for vm-fullscreen). This test
- renders every vm-game record format of src/kernel/probes/f3_probes_video.c
  with the widest value of each conversion and requires the CIUKI_TEST line to
  fit the 240-byte record limit and the key=value grammar;
- feeds a vm-game run (probe records plus the VM lifecycle records the window
  path emits, all tagged vm-game) to the runner's Parser and checks both suite
  cases' expected predicates;
- replays the failing cade4804 prefix and requires the same refusal.
No QEMU; the host ownership check is in tests/host/dosvm_video_test.inc.
"""
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from evidence import Parser, EvidenceError  # noqa: E402

SOURCE = ROOT / 'src/kernel/probes/f3_probes_video.c'
SUITE = ROOT / 'tests/suites/f3-vm.json'
RUN = 'cade4804'
# The widest value of every %s, by the key it fills (closed sets in the probe).
STRING_MAX = {'phase': 'startup', 'when': 'prompt', 'hex': 'f' * 80, 'digest': 'f' * 64,
              'kernel': 'freedos-1.3', 'status': 'FAIL', 'outcome': 'launch-failed'}
WIDEST = {'u': '4294967295', 'd': '-2147483648', 'x': 'ffffffff'}
TOKEN = re.compile(r'[a-z][a-z0-9_]*=[A-Za-z0-9_:.,/+\-]+')


def emitters():
    """(event, format) of every rec_emit(GAME_PROBE, ...) in source order,
    adjacent string literals joined as the compiler does."""
    text = SOURCE.read_text()
    section = text[text.index('#define GAME_PROBE'):]
    found = []
    for match in re.finditer(r'rec_emit\(GAME_PROBE,"([A-Z]+)",((?:\s*"(?:[^"\\]|\\.)*")+)', section):
        fmt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(2)))
        found.append((match.group(1), fmt))
    return found


def render(fmt, values=None):
    """Expand a format; values maps key -> value (or list for repeated
    conversions of one key, e.g. frames=%u/%u); missing keys take the widest."""
    values = values or {}
    used = {}
    def conversion(match):
        key = re.findall(r'([a-z_]+)=[^ =]*$', fmt[:match.start()])
        key = key[-1] if key else ''
        kind = match.group(1)
        if key in values:
            value = values[key]
            if isinstance(value, list):
                index = used.get(key, 0); used[key] = index + 1
                value = value[index]
            return str(value)
        if kind == 's':
            if key not in STRING_MAX:
                raise AssertionError(f'%s for {key!r} has no declared maximum')
            return STRING_MAX[key]
        return WIDEST[kind]
    return re.sub(r'%(?:ll)?([udxs])', conversion, fmt)


def line(seq, event, extra, probe='vm-game', run=RUN):
    return f'CIUKI_TEST v=1 run={run} seq={seq:06d} probe={probe} event={event}' + (' ' + extra if extra else '')


class VmGameRecords(unittest.TestCase):
    def test_every_record_fits_the_line_limit_and_grammar(self):
        found = emitters()
        self.assertGreaterEqual(len(found), 15, found)
        for event, fmt in found:
            text = line(999999, event, render(fmt), run='ffffffff')
            self.assertLessEqual(len(text), 240, text)   # kernel output.c and evidence.py limit
            for token in text.split(' ')[1:]:
                self.assertRegex(token, TOKEN.pattern + '$', text)

    def records(self, kernel):
        """One vm-game run, in the probe's order, with the window path's VM
        records tagged by their owner."""
        formats = {}
        for event, fmt in emitters():
            name = re.match(r'(?:case|action)=([a-z_]+)', fmt)
            formats.setdefault(name.group(1) if name else event, (event, fmt))
        def rec(name, **values):
            event, fmt = formats[name]
            return event, render(fmt, values)
        unified = kernel == 2
        out = [('BEGIN', render(formats['BEGIN'][1], {'kernel': 'unified' if unified else 'freedos-1.3'})),
               # dosvm.c game_create, tagged by window_probe(): the cade4804 culprit.
               ('DATA', 'group=admission free=490942464 reserved=0 resident=0 outstanding=0 request=50331648 '
                        'headroom=33554432 admitted=1'),
               rec('vm', kernel=kernel, expected=kernel, bridge=0),
               rec('console', phase='prompt', row=0, col=0, hex='433a5c3e'),
               rec('unified', when='prompt', kernel=kernel, bound=int(unified), active=int(unified), owned='c' if unified else 0,
                   start=63 if unified else 0, critical=0, refusals=0, held=0),
               rec('unified_io', when='prompt', requests=124, opens=14, closes=14, reads=69, read_kib=89, failed=0, expired=0),
               rec('console', phase='startup', row=1, col=0, hex='444f4f4d'),
               rec('launch', ms=41000, mode=19, unchained=1)]
        out += [rec('frame', index=i, error=0, mode=19, w=320, h=200, digest='a' * 64) for i in range(3)]
        out += [rec('fullscreen_enter'), rec('fullscreen', entered=1, error=0)]
        for second in range(1, 46):
            out.append(rec('sample', t=second * 1000, fs=1, state=1, mode=19, frames=[second, second], dpmi=1, returned=0,
                           cause=0, error=0))
            out.append(rec('progress', t=second * 1000, irq_pm=226 * second, opl=3000 * second, sb=22050 * second,
                           ifs_reads=1183 + second, ifs_failed=4, ifs_critical=0))
            if second == 1: out.append(rec('fullscreen_demo_start'))
            if second == 5: out.append(rec('fullscreen_demo_end'))
        out += [rec('fullscreen_leave'),
                rec('after', state=1, reason=0, vector=0, mode=19, dpmi=1, returned=0, exec=1, idle=0, cause=1),
                rec('unified', when='after', kernel=kernel, bound=int(unified), active=int(unified), owned='c' if unified else 0,
                    start=63 if unified else 0, critical=0, refusals=0, held=1 if unified else 0),
                rec('unified_io', when='after', requests=1365, opens=19, closes=18, reads=1282, read_kib=3034, failed=4, expired=0),
                rec('verdict', outcome='running'),
                # dosvm.c worker and dosvm_destroy at the probe's cleanup.
                ('DATA', 'group=vm_stop vm=1 generation=5469 reason=1 vector=0 eip=00000000 address=00000000 error=0 '
                         'code=0000000000000000'),
                ('DATA', 'group=vm_cleanup vm=1 generation=5469 data_loss=1 error=0'),
                rec('cleanup', restored=1, vms=0, tasks=0, pages=0, handles=0),
                ('END', render(formats['END'][1], {'status': 'PASS', 'error': 0}))]
        return out

    def case(self, name):
        return next(c for c in json.loads(SUITE.read_text())['cases'] if c['id'] == name)

    def test_parser_accepts_both_suite_cases(self):
        for name, kernel in (('vm-game-doom-unified-qemu-t23', 2), ('vm-game-doom-freedos-qemu-t23', 1)):
            case = self.case(name)
            self.assertEqual(case['probe'], 'vm-game')
            parser = Parser(RUN, case['probe'])
            for seq, (event, extra) in enumerate(self.records(kernel), 1):
                text = line(seq, event, extra)
                self.assertLessEqual(len(text), 240, text)
                parser.feed(text.encode() + b'\n')
            self.assertTrue(parser.check(case['expected']), name)

    def test_cade4804_prefix_is_refused(self):
        """The failing QEMU sequence: BEGIN by vm-game, admission by vm-window."""
        parser = Parser(RUN, 'vm-game')
        parser.feed(b'CIUKI_TEST v=1 run=cade4804 seq=000001 probe=vm-game event=BEGIN version=1 game=doom kernel=unified\n')
        with self.assertRaisesRegex(EvidenceError, 'version, run or probe mismatch'):
            parser.feed(b'CIUKI_TEST v=1 run=cade4804 seq=000002 probe=vm-window event=DATA group=admission '
                        b'free=490942464 reserved=0 resident=0 outstanding=0 request=50331648 headroom=33554432 admitted=1\n')

    def test_window_path_owner_names_vm_game(self):
        text = (ROOT / 'src/kernel/vm/dosvm_client.c').read_text()
        self.assertIn('{"f3:vm-game ","vm-game"}', text)
        self.assertIn('{"f3:vm-fullscreen ","vm-fullscreen"}', text)


if __name__ == '__main__':
    unittest.main()
