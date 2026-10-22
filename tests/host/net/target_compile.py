#!/usr/bin/env python3
"""Compile changed kernel boot/delegation and native app TUs; no image/link."""
from pathlib import Path
import runpy
import subprocess
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/host/net-settings/target'
OUT.mkdir(parents=True,exist_ok=True)
flags=runpy.run_path(str(ROOT/'scripts/build_kernel.py'))['CFLAGS']
for name in ('src/kernel/core/init.c','src/kernel/proc/supervisor.c'):
    subprocess.run(['clang',*flags,'-I',str(ROOT/'src/kernel/include'),
        '-I',str(ROOT/'src/kernel/net/lwip/libc'),'-DCIUKI_BUILD_EPOCH=0',
        '-c',str(ROOT/name),'-o',str(OUT/(Path(name).stem+'.o'))],check=True)
for name in ('apps/control/main.c','apps/control/app.c','sdk/libciuki/net_status.c'):
    subprocess.run([str(ROOT/'build/tools/ciuki-sdk/bin/ciuki-cc'),'-O2','-Wall','-Wextra','-Werror',
        '-I',str(ROOT/'sdk/sysroot-overlay/include'),'-I',str(ROOT/'src/kernel/include'),
        '-I',str(ROOT/'apps/desktop'),'-I',str(ROOT/'sdk/win32/include'),
        '-c',str(ROOT/name),'-o',str(OUT/(Path(name).parent.name+'-'+Path(name).stem+'.o'))],check=True)
print('PASS f2-100: kernel boot/delegation and Control Panel/SDK i686 translation units compile with production flags')
