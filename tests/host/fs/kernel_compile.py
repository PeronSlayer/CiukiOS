#!/usr/bin/env python3
"""Extract CFLAGS without importing/executing the kernel builder (or git)."""
import ast
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[3]
tree=ast.parse((root/'scripts/build_kernel.py').read_text())
constants={}
for n in tree.body:
    if isinstance(n,ast.Assign) and len(n.targets)==1 and isinstance(n.targets[0],ast.Name):
        try: constants[n.targets[0].id]=ast.literal_eval(n.value)
        except (ValueError,TypeError): pass
def literal(node):
    if isinstance(node,ast.JoinedStr):
        parts=[]
        for part in node.values:
            if isinstance(part,ast.Constant) and isinstance(part.value,str): parts.append(part.value)
            elif (isinstance(part,ast.FormattedValue) and isinstance(part.value,ast.Name)
                  and part.conversion==-1 and part.format_spec is None):
                parts.append(str(constants[part.value.id]))
            else: raise ValueError('non-constant kernel flag')
        return ''.join(parts)
    return ast.literal_eval(node)
flag_list=next(n.value for n in tree.body if isinstance(n,ast.Assign)
               and any(isinstance(t,ast.Name) and t.id=='CFLAGS' for t in n.targets))
assert isinstance(flag_list,ast.List)
flags=[literal(n) for n in flag_list.elts]
out=root/'build/host/fs/kernel'
out.mkdir(exist_ok=True)
sources=sorted((root/'src/kernel/fs').glob('*.c'))
sources += [root/'src/kernel/core/bootlog.c', root/'src/kernel/core/blackbox.c', root/'src/kernel/probes/fat_probes.c']
# f1-39: compile the native string-I/O boundary and enlarged file-read path
# with the real target flags, including the unchanged 4096-byte frame guard.
sources += [root/'src/kernel/drivers/ata.c', root/'src/kernel/drivers/ata_dma.c', root/'src/kernel/drivers/ata_probe.c',
            root/'src/kernel/proc/fdtable.c', root/'src/kernel/proc/syscalls_file.c', root/'src/kernel/proc/syscalls_proc.c']
sources += [root/'src/kernel/probes/f2_probes_files.c']
sources += [root/'src/kernel/probes/f2_probes_desktop.c', root/'src/kernel/probes/selector.c']
sources += sorted((root/'src/kernel/usb').glob('*.c'))
sources += [root/'src/kernel/drivers/blkpart.c', root/'src/kernel/drivers/media_dev.c', root/'src/kernel/proc/posixpath.c', root/'src/kernel/proc/devnodes.c']
sources += [root/'src/kernel/drivers/optical.c', root/'src/kernel/drivers/optical_cd.c', root/'src/kernel/drivers/media_cd.c']
# F0-23: boot-only calibration, UART bounds, port pacing and live RO inventory.
sources += [root/'src/kernel/core'/name for name in ('serial.c', 'output.c', 'timing.c', 'main.c', 'devices.c')]
sources += [root/'src/kernel/drivers/i8042.c']
# f3-49b: compile the production DOS adapter/teardown with the target ABI.
sources += [root/'src/kernel/vm'/name for name in
            ('dos_file.c', 'dos_file_volume.c', 'dos_file_vm.c', 'dosvm_disk.c', 'dosvm.c')]
for source in sources:
    subprocess.run(['clang',*flags,'-I'+str(root/'src/kernel/include'),'-fstack-usage','-c',str(source),
                    '-o',str(out/(source.stem+'.o'))],check=True)
print(f'PASS kernel compile: {len(sources)} translation units; exact scripts/build_kernel.py CFLAGS')
# A kernel link is outside this directive; flag newly required runtime helpers.
undefined=set()
for obj in out.glob('*.o'):
    for line in subprocess.check_output(['nm','-u',str(obj)],text=True).splitlines():
        undefined.add(line.split()[-1])
# lib/divdi3.c exports the 64-bit division helpers clang emits on i386
# (execution-abi.md, "Runtime helpers"); ntfs_ro.c divides 64-bit values.
provided={'__stack_chk_guard','__stack_chk_fail','__udivdi3','__umoddi3','__divdi3','__moddi3'}
helpers=sorted(x for x in undefined if x.startswith('__') and x not in provided)
if helpers: raise SystemExit('unexpected kernel compiler runtime helpers: '+', '.join(helpers))
print('PASS kernel objects: no compiler runtime helpers beyond the 64-bit division set of lib/divdi3.c')
