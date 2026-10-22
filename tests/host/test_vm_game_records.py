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
              'kernel': 'freedos-1.3', 'status': 'FAIL', 'outcome': 'launch-failed',
              # f3-61c
              'continuation': 'windowed', 'policy': 'hardware', 'display': 'fullscreen',
              'method': 'idle-prompt', 'why': 'game-running', 'name': 'X' * 12,
              # f3-61d
              'threshold': 'qemu-tcg'}
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
        kind = match.group(2)
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
    return re.sub(r'%(0?[0-9]*)(?:ll)?([udxs])', conversion, fmt)


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

    def records(self, kernel, windowed=False):
        """One vm-game run, in the probe's order, with the window path's VM
        records tagged by their owner. windowed: QEMU refuses full screen
        (f3-29 std-VGA readback) and the probe verifies the windowed game."""
        formats = {}
        for event, fmt in emitters():
            name = re.match(r'(?:case|action)=([a-z_]+)', fmt)
            formats.setdefault(name.group(1) if name else event, (event, fmt))
        def rec(_record, **values):
            event, fmt = formats[_record]
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
        out += [rec('fullscreen_enter'),
                rec('fullscreen', entered=int(not windowed), refused=int(windowed), reason=-5 if windowed else 0,
                    continuation='windowed' if windowed else 'none', policy='qemu', error=0)]
        for second in range(1, 26 if windowed else 46):
            out.append(rec('sample', t=second * 1000, fs=int(not windowed), state=1, mode=19, frames=[second, second],
                           dpmi=1, returned=0, cause=0, error=0))
            out.append(rec('progress', t=second * 1000, irq_pm=226 * second, opl=3000 * second, sb=22050 * second,
                           flips=second // 8 if windowed else 35 * second, ifs_reads=1183 + second, ifs_failed=4,
                           ifs_critical=0))
            if second == 1: out.append(rec('fullscreen_demo_start'))
            if second == 5: out.append(rec('fullscreen_demo_end'))
        if not windowed: out.append(rec('fullscreen_leave'))
        out += [rec('verdict', outcome='running', display='window' if windowed else 'fullscreen',
                    threshold='qemu-tcg' if windowed else 'frames'),
                rec('after', state=1, reason=0, vector=0, mode=3, dpmi=0, returned=1, exec=0, idle=1, cause=1),
                rec('console', phase='after', row=24, col=0, hex='433a5c444f4f4d3e'),
                rec('unified', when='after', kernel=kernel, bound=int(unified), active=int(unified), owned='c' if unified else 0,
                    start=63 if unified else 0, critical=0, refusals=0, held=0)]
        if unified:
            out += [rec('unified_io', when='after', requests=1365, opens=20, closes=20, reads=1282, read_kib=3034,
                        failed=4, expired=0)]
            out += [rec('unified_failed', when='after', n=i + 1, of=4, fn=fn, dos=dos, error=2, name=name)
                    for i, (fn, dos, name) in enumerate((('110f', '43', 'DOOM2F.WAD'), ('110f', '43', 'DOOM2.WAD'),
                                                         ('110f', '43', 'DOOM.WAD'), ('1116', '3d', 'DOOM.ETX')))]
        else:
            out += [rec('unified_io', when='after', requests=0, opens=0, closes=0, reads=0, read_kib=0, failed=0, expired=0)]
        out += [rec('close', method='idle-prompt', why='none'),
                # dosvm.c worker and dosvm_destroy at the probe's cleanup: an idle shell exits.
                ('DATA', 'group=vm_stop vm=1 generation=5469 reason=0 vector=0 eip=00000000 address=00000000 error=0 '
                         'code=0000000000000000'),
                ('DATA', 'group=vm_cleanup vm=1 generation=5469 data_loss=0 error=0'),
                rec('cleanup', restored=1, vms=0, tasks=0, pages=0, handles=0),
                ('END', render(formats['END'][1], {'status': 'PASS', 'error': 0}))]
        return out

    def case(self, name):
        return next(c for c in json.loads(SUITE.read_text())['cases'] if c['id'] == name)

    def test_parser_accepts_both_suite_cases(self):
        for name, kernel in (('vm-game-doom-unified-qemu-t23', 2), ('vm-game-doom-freedos-qemu-t23', 1)):
            for windowed in (False, True):
                case = self.case(name)
                self.assertEqual(case['probe'], 'vm-game')
                parser = Parser(RUN, case['probe'])
                for seq, (event, extra) in enumerate(self.records(kernel, windowed), 1):
                    text = line(seq, event, extra)
                    self.assertLessEqual(len(text), 240, text)
                    parser.feed(text.encode() + b'\n')
                self.assertTrue(parser.check(case['expected']), (name, windowed))
                self.assertEqual(self.stimuli(case, parser.records), not windowed, (name, windowed))

    def stimuli(self, case, records):
        """Drive the runner's Actions over the recorded run; True when the
        conditional leave stimulus was sent, False when it was released."""
        import run as runner

        class Qmp:
            def __init__(self): self.sent = []
            def input_events(self, events): self.sent.append(events)

        class Records:
            def __init__(self): self.records = []

        actions, qmp, seen, now = runner.Actions(case['actions']), Qmp(), Records(), 0.0
        for record in records:
            seen.records.append(record)
            for _ in range(10):
                now += 0.2; actions.step(seen, qmp, now)
        actions.skip_declined(seen)
        self.assertTrue(actions.complete)
        skipped = [o for o in actions.observed if o['type'] == 'skipped']
        self.assertLessEqual(len(skipped), 1)
        return not skipped

    def test_missing_arm_without_its_declining_record_stays_incomplete(self):
        """A probe that drops ARM fullscreen_leave without case=fullscreen
        entered=0 must still fail the declared stimuli."""
        import run as runner
        case = self.case('vm-game-doom-unified-qemu-t23')
        records = [r for r in self.parsed(2, windowed=False) if r.get('action') != 'fullscreen_leave']
        class Seen: pass
        seen = Seen(); seen.records = []
        class Qmp:
            def input_events(self, events): pass
        actions, now = runner.Actions(case['actions']), 0.0
        for record in records:
            seen.records.append(record)
            for _ in range(10):
                now += 0.2; actions.step(seen, Qmp(), now)
        actions.skip_declined(seen)
        self.assertFalse(actions.complete)

    def parsed(self, kernel, windowed):
        parser = Parser(RUN, 'vm-game')
        for seq, (event, extra) in enumerate(self.records(kernel, windowed), 1):
            parser.feed(line(seq, event, extra).encode() + b'\n')
        return parser.records

    def test_freedos_case_request_reaches_the_kernel(self):
        """f3-61d: the runner rebuilt the fw_cfg request without the vm-game
        kernel suffix, so the freedos case launched kernel=unified."""
        import ctypes, subprocess, tempfile
        import run as runner
        out = ROOT / 'build/host'; out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='vm-game-selector-', dir=out) as folder:
            shim = Path(folder) / 'shim.c'
            shim.write_text(r'''
#include <ciuki/kernel.h>
#include "selector.h"
void rec_emit(const char *p, const char *e, const char *f, ...) { (void)p;(void)e;(void)f; }
void klog(const char *f, ...) { (void)f; }
int kernel_of(const char *s, unsigned n, unsigned flags) {
    struct probe_selection selection;
    if (!probes_parse_selector(s, n, flags, &selection)) return -1;
    return (selection.flags & PROBE_DOS_FREEDOS) ? 1 : (selection.flags & PROBE_DOS_UNIFIED) ? 2 : 0;
}
''')
            lib = Path(folder) / 'selector.so'
            subprocess.run(['clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/probes'), str(shim),
                            str(ROOT / 'src/kernel/probes/selector.c'), '-o', str(lib)], check=True)
            native = ctypes.CDLL(str(lib))
            native.kernel_of.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint]
            qemu_flag = 1 << 3  # CBI_F_SMBIOS_QEMU, only needed by a platform suffix
            for name, kernel, profile in (('vm-game-doom-freedos-qemu-t23', 1, 'qemu-t23'),
                                          ('vm-game-doom-unified-qemu-t23', 0, 'qemu-t23'),
                                          ('vm-game-doom-freedos-qemu-t23', 1, 'qemu-e500')):
                case = self.case(name)
                _, request = runner.qemu_args('fake', runner.load(ROOT / f'tests/profiles/{profile}.json'), case,
                                              '12345678', Path(folder) / 'overlay', Path(folder) / 'bios')
                if kernel == 1: self.assertIn(' kernel=freedos-1.3', request)
                code = native.kernel_of(request.encode(), len(request), 0xFFFFFFFF)
                self.assertEqual(code, kernel, request)

    def test_windowed_demo_compares_the_dos_window(self):
        """f3-61d: a refused full screen skips the demo pair and compares
        desktop screendumps >= 5 s apart for a change inside a DOS window."""
        import tempfile
        import run as runner
        width, height = 1024, 768
        def frame(seed):
            pixels = bytearray(b'\x40\x80\xc0' * width * height)
            if seed:
                for y in range(100, 500):
                    for x in range(200, 840, 7): pixels[(y * width + x) * 3] = seed
            return b'P6\n%d %d\n255\n' % (width, height) + bytes(pixels)
        out = ROOT / 'build/host'; out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='vm-game-window-', dir=out) as folder:
            folder = Path(folder)
            for frames, expected in (((0, 0, 0, 9), True), ((0, 0, 0, 0, 0), False)):
                class Qmp:
                    def __init__(self): self.shots = list(frames)
                    def command(self, name, args):
                        assert name == 'screendump'
                        Path(args['filename']).write_bytes(frame(self.shots.pop(0)))
                class Seen: records = []
                seen, qmp, demo = Seen(), Qmp(), runner.WindowDemo(folder)
                seen.records = [{'event': 'DATA', 'case': 'fullscreen', 'continuation': 'windowed'},
                                {'event': 'ARM', 'action': 'fullscreen_demo_start'}]
                self.assertTrue(runner.WindowDemo.windowed(seen))
                now = 0.0
                while now < 30.0 and qmp.shots and not (demo.result and demo.result['changed']):
                    demo.step(seen, qmp, now); now += 0.5
                self.assertEqual(bool(demo.result and demo.result['changed']), expected, demo.result)
                if expected:
                    self.assertGreaterEqual(demo.result['seconds'], 5.0)
                    self.assertLessEqual(demo.result['box'][2], 640); self.assertLessEqual(demo.result['box'][3], 480)
                # After the verdict no further screendump is taken (DOOM's ENDOOM is not game progress).
                seen.records.append({'event': 'DATA', 'case': 'verdict'})
                left = len(qmp.shots); demo.step(seen, qmp, now + 100); self.assertEqual(len(qmp.shots), left)
            # A change wider than one DOS window client (desktop repaint) is not window progress.
            before, after = folder / 'a.ppm', folder / 'b.ppm'
            before.write_bytes(frame(0)); wide = bytearray(frame(0)); wide[-3] = 1; wide[len(b'P6\n1024 768\n255\n')] = 1
            after.write_bytes(bytes(wide))
            self.assertFalse(runner.observe_window_demo(before, after, 5.0, 1)['changed'])

    def test_hardware_refusal_fails_the_verdict(self):
        """policy=hardware: the T23 expectation keeps full screen required."""
        text = SOURCE.read_text()
        self.assertIn('if (!entered && !(refused && qemu)) { error=-EIO; outcome=refused ? "refused" : "not-entered"; goto observed; }', text)
        self.assertIn('bool qemu=(g_boot.flags&CBI_F_SMBIOS_QEMU)!=0;', text)

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
