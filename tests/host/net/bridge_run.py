#!/usr/bin/env python3
"""Focused physical DOS bridge boundary, including administrative disable."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/host/net-settings/bridge'
OUT.mkdir(parents=True,exist_ok=True)
pool=subprocess.check_output(['python3',str(ROOT/'tests/host/vm/pool_build.py'),str(ROOT),str(OUT)],text=True).strip()
binary=OUT/'port-test'
subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread',
    '-I',str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/vm/port_test.c'),
    str(ROOT/'src/kernel/net/bridge.c'),str(ROOT/'src/kernel/net/ipx_config.c'),str(ROOT/'src/kernel/lib/fmt.c'),
    pool,'-Wl,-u,pool_fixture_start','-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
