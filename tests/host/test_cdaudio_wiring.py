"""fx-10: Quake's music path reaches the VMM's MSCDEX.

T23 trial of preview 423b803a: Quake played sound effects but no music.
Quake's CDAudio_Init (WinQuake cd_audio.c) issues INT 2Fh AX=1500h with
BX=0 through DPMI 0300h (dos_int86 -> _go32_dpmi_simulate_int) and disables
its music when BX is still 0 ("MSCDEX not loaded"). Before fx-10 no layer
answered AH=15h: the monitor reflected the call to the guest's FreeDOS
INT 2Fh handler, whose dispatcher has no AH=15h case and IRETs. This test
pins the routing that makes 1500h/1510h reach dosvm_cdaudio.c on both
entries, the lifecycle calls, and the record widths; the request sequence
itself is replayed by tests/host/vm/cdaudio_test.c.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / 'src/kernel/vm'


def function_body(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[brace:i + 1]
    raise AssertionError(signature)


class CdAudioWiring(unittest.TestCase):
    def test_freedos_int2f_has_no_mscdex(self):
        # The root cause: the guest chain cannot answer AH=15h by itself.
        source = (ROOT / 'third_party/freedos/kernel-master/kernel/int2f.asm').read_text(errors='replace')
        handled = set(re.findall(r'cmp\s+ah\s*,\s*([0-9a-f]+)h', source, re.I))
        self.assertTrue({'11', '12', '16'} <= {h.lower() for h in handled})
        self.assertNotIn('15', {h.lower() for h in handled})

    def test_v86_int2f_reaches_mscdex_first(self):
        body = function_body((VM / 'dosvm.c').read_text(), 'static int interrupt_dispatch(')
        cd = body.index('dosvm_cdaudio_interrupt(v, f)')
        self.assertLess(cd, body.index('dosvm_network_interrupt(v, f, vector)'))
        self.assertLess(cd, body.index('dosvm_file_interrupt(v,f,vector,0)'))
        self.assertLess(cd, body.index('dpmi_v86_int(v, f, vector)'))

    def test_dpmi_0300_int2f_reaches_mscdex_before_the_ivt(self):
        body = function_body((VM / 'dpmi_switch.c').read_text(), 'int dpmi_translation(')
        cd = body.index('dosvm_cdaudio_reflect(v,&n,image)')
        self.assertIn('vector==0x2f && dosvm_cdaudio_reflect', body)
        self.assertLess(cd, body.index('dosvm_file_reflect(v,&n,vector,1,image)'))
        self.assertIn('cd==-ENOSYS && ax==0x0300 && dosvm_file_reflect', body)
        self.assertLess(cd, body.index('dpmi_set_pm(v, false)'))

    def test_lifecycle(self):
        client = (VM / 'dosvm_client.c').read_text()
        self.assertLess(client.index('dosvm_cdaudio_open(v)'), client.index('e = dosvm_boot(v)'))
        self.assertLess(client.index('memcpy(v->name, c.name'), client.index('dosvm_cdaudio_open(v)'))
        destroy = function_body((VM / 'dosvm.c').read_text(), 'int dosvm_destroy(')
        close = destroy.index('dosvm_cdaudio_close(v)')
        self.assertLess(destroy.index('task_reap(v->task)'), close)
        self.assertLess(close, destroy.index('as_destroy(&v->as)'))

    def test_record_widths(self):
        # Every record rendered with 10-digit numbers and the widest strings
        # fits the 240-byte record line.
        source = (VM / 'dosvm_cdaudio.c').read_text()
        formats = re.findall(r'klog\("((?:[^"\\]|\\.)*)"', source)
        self.assertGreaterEqual(len(formats), 15)
        for fmt in formats:
            line = re.sub(r'%0?\d*u', '4294967295', fmt)
            line = re.sub(r'%d', '-2147483648', line)
            # %s: paths are bounded by their buffers (dir[40], path[64]);
            # the other strings are short closed sets (result, reason, stage).
            line = re.sub(r'(\w+)=%s', lambda m: m.group(1) + '=' + 'X' * {'file': 63, 'source': 39}.get(m.group(1), 16), line)
            self.assertNotIn('%s', line, fmt)
            line = line.replace('%c', 'Z')
            self.assertLessEqual(len(line.encode()), 240, fmt)


if __name__ == '__main__':
    unittest.main()
