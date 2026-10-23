"""Sequential family register evidence. No guest/image build.

UBSan by default. CIUKI_DISPLAY_ASAN=1 adds ASan with its default leak check.
"""
from pathlib import Path
import os
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SavageFamilyTests(unittest.TestCase):
    def test_family_registers(self):
        out = ROOT / 'build/host/savage-family'
        out.mkdir(parents=True, exist_ok=True)
        binary = out / 'test'
        # Rename only the fixture entry point. Its nested fbdev fixture owns
        # its own main macro, so an outer -Dmain cannot do this safely.
        source = (ROOT / 'tests/host/savage2d_test.c').read_text()
        signature = '\nint main(void)\n'
        self.assertEqual(source.count(signature), 1)
        source = source.replace(signature, '\nint savage_regression_main(void)\n')
        source = source.replace('static unsigned binding_claims,',
                                '#include "savage_family_resources.h"\nstatic unsigned binding_claims,', 1)
        # Extend only resource/MMIO stubs, leaving all existing behavioural
        # assertions intact and using their original branch outside lifecycle
        # cases. The standalone legacy suite still compiles the original file.
        boundaries = {
            'binding_claim': 'family_claim(t,s,e,o,share)',
            'binding_get': 'family_get(i)',
            'binding_activate': 'family_activate(h,gen)',
            'binding_quarantine': 'family_quarantine(h,gen)',
            'binding_quiesce': 'family_quiesce(h,gen,proof)',
            'binding_release': 'family_release(h,gen)',
            'binding_offer_takeover': 'family_takeover(h,gen,successor,proof)',
            'binding_reserve': 'family_reserve(capacity)',
            'vmm_replace_mmio': 'family_map(w,c,p,n)',
        }
        for name, call in boundaries.items():
            pattern = r'(?m)^(?:static )?[^\n]*\b' + name + r'\([^{};]*\)\s*\{'
            matches = list(re.finditer(pattern, source))
            self.assertEqual(len(matches), 1, name)
            end = matches[0].end()
            source = source[:end] + '\n    if (family_resources) return ' + call + ';\n' + source[end:]
        legacy_paths = '(!strcmp(path,"C:/system/settings/accel-s3savage-5333-8c2e.cfg") || !strcmp(path,"C:/system/settings/accel-s3savage-5333-8a22.cfg"))'
        self.assertEqual(source.count(legacy_paths), 1)
        source = source.replace(legacy_paths, '(' + legacy_paths +
                                ' || (family_resources && family_marker_path(path)))')
        (out / 'legacy_savage_fixture.inc').write_text(source)
        sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                        '-I', str(ROOT / 'src/kernel/include'),
                        '-I', str(ROOT / 'tests/host'), '-I', str(out),
                        str(ROOT / 'tests/host/savage_family_test.c'),
                        str(ROOT / 'src/kernel/lib/fmt.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
