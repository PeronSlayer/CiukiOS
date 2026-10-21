#!/usr/bin/env python3
"""Focused real-stack and target compile checks. Run with nice -n 19."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from pathlib import Path
import runpy
import re
import json
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
recipe = runpy.run_path(str(ROOT / 'src/kernel/net/lwip/build.py'))
source = recipe['source']()
PORT = ROOT / 'src/kernel/net/lwip'
OUT = ROOT / 'build/host/lwip'
OUT.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser()
parser.add_argument('--kernel', action='store_true')
args = parser.parse_args()
flags = runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))['CFLAGS'] if args.kernel else [
    '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DLWIP_HOST', '-pthread',
    '-ffunction-sections', '-fdata-sections']
includes = recipe['includes'](source) + ['-I', str(ROOT / 'src/kernel/include')]
if args.kernel:
    includes += ['-I', str(PORT / 'libc')]
files = [source / 'src' / n for n in recipe['SOURCES']] + [PORT / n for n in (
    'sys_arch.c', 'stack.c', 'sockets.c', 'syscalls.c')]
if not args.kernel:
    files += [ROOT / 'src/kernel/net/bridge.c', ROOT / 'src/kernel/net/packet_pool.c']
if args.kernel:
    files += [ROOT / n for n in ('src/kernel/net/lwip_handoff.c', 'src/kernel/core/syscall.c',
        'src/kernel/proc/fdtable.c', 'src/kernel/proc/channel.c', 'src/kernel/proc/grants.c',
        'src/kernel/proc/surface.c', 'src/kernel/proc/syscalls_file.c',
        'src/kernel/drivers/net.c', 'src/kernel/core/ledger.c')]
    files += [ROOT / ('src/kernel/drivers/' + name + '.c') for name in (
        'net_dev', 'net_arp', 'net_e100', 'net_rtl8139', 'net_ne2000', 'net_e1000',
        'net_pcnet', 'net_tulip', 'net_3c90x', 'net_rhine')]
objects = []
for index, path in enumerate(files):
    obj = OUT / (('target-' if args.kernel else 'host-') + str(index) + '.o')
    result = subprocess.run(['clang', *flags, *includes, *(['-Xclang', '-fdump-record-layouts'] if args.kernel else []),
        '-MMD', '-MF', str(obj.with_suffix('.d')), '-c', str(path), '-o', str(obj)], capture_output=args.kernel, text=True)
    if result.returncode:
        if args.kernel: sys.stderr.write(result.stdout + result.stderr)
        result.check_returncode()
    if args.kernel:
        with (OUT / 'target-layouts.txt').open('a' if index else 'w') as stream:
            stream.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
    objects.append(obj)
if args.kernel:
    subprocess.run(['ld.lld', '-r', *map(str, objects), '-o', str(OUT / 'target.o')], check=True)
    layouts = (OUT / 'target-layouts.txt').read_text()
    sizes = {}
    for block in layouts.split('*** Dumping AST Record Layout'):
        name = re.search(r'^\s*0 \| (?:struct|union) (\w+)', block, re.M)
        size = re.search(r'\[sizeof=(\d+), align=(\d+)\]', block)
        if name and size:
            sizes[name[1]] = {'bytes': int(size[1]), 'alignment': int(size[2])}
    wanted = ('pbuf', 'tcp_pcb', 'udp_pcb', 'netconn', 'socket_object', 'cu_mbox',
              'cu_sem', 'cu_mutex', 'request', 'user_operation', 'poll_operation',
              'dns_request', 'ethernet_port', 'task')
    measured = {n: sizes[n] for n in wanted if n in sizes}
    network_objects = objects[:33] + [objects[39]] + objects[41:]
    subprocess.run(['ld.lld', '-r', *map(str, network_objects),
        '-Map=' + str(OUT / 'network.map'), '-o', str(OUT / 'network.o')], check=True)
    runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))['audit'](OUT / 'network.o')
    nm = subprocess.check_output(['llvm-nm', '--print-size', '--size-sort', *map(str, network_objects)], text=True)
    bss = {match[2]: int(match[1], 16) for match in re.finditer(
        r'^[0-9a-f]+ ([0-9a-f]+) [bB] (\S+)$', nm, re.M)}
    measured['static_objects'] = bss
    measured['static_bss_bytes'] = sum(int(m[1], 16) for m in re.finditer(
        r'^[0-9a-f]+ ([0-9a-f]+) [bB] (\S+)$', nm, re.M))
    size_lines = subprocess.check_output(['llvm-size', str(OUT / 'network.o')], text=True).splitlines()
    text_bytes, data_bytes, bss_bytes = map(int, size_lines[-1].split()[:3])
    measured['linked_sections'] = {'text_rodata': text_bytes, 'data': data_bytes, 'bss': bss_bytes}
    # Eight tasks, two guarded NIC DMA regions, and the deferred bridge budget.
    # Leave page/alignment and code rounding headroom inside the approved 4 MiB.
    upper = sum((n + 4095) // 4096 * 4096 for n in (text_bytes, data_bytes, bss_bytes))
    upper += 288 * 1024 + 256 * 1024 + 256 * 1024
    # New common fd generations add 512 bytes/process. Allow a full extra
    # allocator page for each of the 64 process slots, plus one ledger page.
    upper += 64 * 4096 + 4096
    measured['rounded_ceiling_proof_bytes'] = upper
    if upper > 4 * 1024 * 1024:
        raise ValueError('network static/task/DMA/deferred-bridge proof exceeds 4 MiB')
    if bss['ram_heap'] > 1028 * 1024:
        raise ValueError('network heap bucket exceeded')
    if sum(n for name, n in bss.items() if name.startswith('memp_memory_')) > 508 * 1024:
        raise ValueError('network fixed-pool bucket exceeded')
    if 8 * 32768 + bss['io_buffers'] != 512 * 1024:
        raise ValueError('copied I/O exceeds the 512 KiB bucket')
    metadata = sum(bss[name] for name in ('sems', 'mutexes', 'mboxes', 'threads',
        'sockets', 'polls', 'lookups', 'operations')) + bss['requests'] - 8 * 32768 + 4096
    measured['port_metadata_with_global_rounding'] = metadata
    if metadata > 256 * 1024:
        raise ValueError('sys_arch/socket metadata bucket exceeded')
    print('CEILING static/code/rounding + eight tasks + two DMA regions + deferred bridge + common metadata =', upper, 'bytes < 4194304')
    (OUT / 'target-sizes.json').write_text(json.dumps(measured, indent=2, sort_keys=True) + '\n')
    for name in wanted:
        if name in measured: print('SIZE', name, measured[name]['bytes'])
    print('PASS: production flags,', len(files), 'translation units; relocatable link; i686 sizeof/BSS measurements')
else:
    binary = OUT / 'stack-test'
    subprocess.run(['clang', *flags, *includes, str(ROOT / 'tests/host/lwip/stack_test.c'),
        str(ROOT / 'tests/host/lwip/channel_fixture.c'),
        *map(str, objects), '-Wl,--gc-sections', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=90)
