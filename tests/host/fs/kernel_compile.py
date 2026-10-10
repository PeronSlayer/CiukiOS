#!/usr/bin/env python3
"""Extract CFLAGS without importing/executing the kernel builder (or git)."""
import ast
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[3]
tree=ast.parse((root/'scripts/build_kernel.py').read_text())
flags=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
           and any(isinstance(t,ast.Name) and t.id=='CFLAGS' for t in n.targets))
out=root/'build/host/fs/kernel'
out.mkdir(exist_ok=True)
sources=sorted((root/'src/kernel/fs').glob('*.c'))
sources += [root/'src/kernel/core/bootlog.c', root/'src/kernel/probes/fat_probes.c']
for source in sources:
    subprocess.run(['clang',*flags,'-I'+str(root/'src/kernel/include'),'-fstack-usage','-c',str(source),
                    '-o',str(out/(source.stem+'.o'))],check=True)
print(f'PASS kernel compile: {len(sources)} translation units; exact scripts/build_kernel.py CFLAGS')
# A kernel link is outside this directive; flag newly required runtime helpers.
undefined=set()
for obj in out.glob('*.o'):
    for line in subprocess.check_output(['nm','-u',str(obj)],text=True).splitlines():
        undefined.add(line.split()[-1])
helpers=sorted(x for x in undefined if x.startswith('__') and x not in {'__stack_chk_guard','__stack_chk_fail'})
if helpers: raise SystemExit('unexpected kernel compiler runtime helpers: '+', '.join(helpers))
print('PASS kernel objects: no compiler runtime helpers')
