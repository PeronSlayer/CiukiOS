"""Compile-time heap bounds, independent of optimizer and target pointer width."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/host'


class HeapLimits(unittest.TestCase):
    def test_constant_guards(self):
        OUT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            source = Path(tmp) / 'guard.c'
            for compiler in ('clang', 'gcc'):
                for opt in ('-O0', '-O2'):
                    for allocator in ('kmalloc', 'kzalloc'):
                        for size, valid in (('0', True), ('KHEAP_MAX_REQUEST', True),
                                            ('KHEAP_MAX_REQUEST+1', False),
                                            ('sizeof(char[KHEAP_CLASS_MAX])', False),
                                            ('(size_t)-1', False), ('-1LL', False), ('n++', True)):
                            source.write_text('#include <ciuki/mm.h>\n'
                                              f'void *check(size_t n) {{ return {allocator}({size}); }}\n')
                            cmd = [compiler, '-std=c17', opt, '-Werror',
                                   '-I', str(ROOT/'src/kernel/include'), '-c', str(source),
                                   '-o', str(Path(tmp)/'guard.o')]
                            result = subprocess.run(cmd, capture_output=True, text=True)
                            self.assertEqual(result.returncode == 0, valid,
                                             f'{cmd}: {size}\n{result.stderr}')
                            if not valid:
                                self.assertIn('negative', result.stderr)

    def test_single_evaluation(self):
        OUT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            source = Path(tmp)/'once.c'
            source.write_text('''#include <ciuki/mm.h>
static size_t seen;
void *(kmalloc)(size_t n) { seen=n; return 0; }
void *(kzalloc)(size_t n) { seen=n; return 0; }
int main(void) {
    size_t n=1;
    (void)kmalloc(n++);
    if (n!=2 || seen!=1) return 1;
    (void)kzalloc(n++);
    return n!=3 || seen!=2;
}
''')
            binary = Path(tmp)/'once'
            for compiler in ('clang', 'gcc'):
                subprocess.run([compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
                                '-I', str(ROOT/'src/kernel/include'), str(source),
                                '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
