#!/usr/bin/env python3
"""Host NETWORK.INI production codec and VFS error handling; ASan/UBSan."""
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/host/net-settings'
OUT.mkdir(parents=True,exist_ok=True)
binary=OUT/'settings-test'
sources=['tests/host/net/settings_test.c','src/kernel/net/config.c','src/kernel/net/settings.c','src/kernel/lib/fmt.c']
subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
    '-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-DFS_HOST','-D_POSIX_C_SOURCE=200809L',
    '-I',str(ROOT/'src/kernel/include'),*[str(ROOT/p) for p in sources],'-o',str(binary)],check=True)
capability=OUT/'capability-test'
subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
    '-I',str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/net/capability_test.c'),
    str(ROOT/'src/kernel/proc/supervisor.c'),'-o',str(capability)],check=True)
failed=False
for test in (binary,capability):
    result=subprocess.run([str(test)])
    failed |= result.returncode != 0
sys.exit(int(failed))
