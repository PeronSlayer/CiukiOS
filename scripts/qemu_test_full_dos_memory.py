#!/usr/bin/env python3
"""Check DOS allocation semantics and secondary COMMAND.COM behavior."""
import argparse
from pathlib import Path
import shutil
import subprocess
from qemu_test_full_display_profile import VM

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path, default=Path('build/full/qemu-dos-memory'))
ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
ap.add_argument('--command', type=Path)
args = ap.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
disk = out / 'test.img'
shutil.copyfile(args.image, disk)
for name, source in [('MEMTEST', 'memory'), ('CMDTEST', 'command_status')]:
    subprocess.run(['nasm', '-f', 'bin', f'src/probes/doscompat/{source}.asm',
                    '-o', str(out / (name + '.COM'))], check=True)
(out / 'EXIT37.COM').write_bytes(bytes.fromhex('b8254ccd21'))
for name in ('MEMTEST', 'CMDTEST', 'EXIT37'):
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(out / (name + '.COM')),
                    f'::APPS/{name}.COM'], check=True)
if args.command:
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(args.command),
                    '::COMMAND.COM'], check=True)
vm = VM(disk, out)
try:
    vm.wait('CiukiOS SHELL C:\\APPS>')
    vm.command('memtest', 'MEMORY PASS')
    print('[dos-memory] PASS initial PSP ceiling, actual MCB size and allocation isolation', flush=True)
    vm.command('cmdtest', 'COMMAND STATUS PASS')
    print('[dos-memory] PASS COMMAND /C returns child exit code 37', flush=True)
    offset = vm.offset()
    vm.text('c:\\command.com /k echo KEEPOPEN')
    vm.wait('KEEPOPEN\r\nCiukiOS SHELL C:\\APPS>', offset)
    vm.text('echo SECONDARY ACTIVE')
    vm.wait('SECONDARY ACTIVE\r\nCiukiOS SHELL', offset)
    offset = vm.offset()
    vm.text('exit')
    vm.wait('CiukiOS SHELL C:\\APPS>', offset)
    vm.command('echo PARENT ACTIVE', 'PARENT ACTIVE\r\nCiukiOS SHELL')
    print('[dos-memory] PASS COMMAND /K stays interactive and EXIT resumes parent', flush=True)
finally:
    vm.shot('final')
    vm.close()
