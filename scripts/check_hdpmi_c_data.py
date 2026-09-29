#!/usr/bin/env python3
"""Refuse C objects linked into HDPMI that read their own data through DS.

HDPMI calls the CiukiOS C code in ring 0 with a flat DS (base 0), while the
objects' data offsets are relative to HDPMI's image. Such a reference reads
low DOS memory instead (the SR4/GR6 write masks once did: DOOM's Mode-Y
writes lost bits and the model stopped reporting a geometry). Only CS-relative
code and flat pointers passed in are safe.

The mode table is used solely by the BIOS mode set, which inside HDPMI is
bridged to the JLM; that exception holds only while no HDPMI object calls
those functions.
"""
import re
import subprocess
import sys

ALLOWED = {('_modes', '_cvga_find_mode'), ('_modes', '_cvga_set_bios_mode')}
BIOS_ONLY = ('cvga_find_mode', 'cvga_set_bios_mode', 'cvga_init')


def disassemble(obj, wdis):
    return subprocess.run([wdis, obj], check=True, capture_output=True, text=True).stdout.splitlines()


def main():
    wdis, objects = sys.argv[1], sys.argv[2:]
    problems = []
    listings = {obj: disassemble(obj, wdis) for obj in objects}
    for obj, lines in listings.items():
        data, labels = set(), []
        for i, line in enumerate(lines):
            label = re.match(r'^[0-9A-F]{4}\t+(\w+):$', line)
            if label:
                labels.append((i, label.group(1)))
                following = lines[i + 1] if i + 1 < len(lines) else ''
                if re.match(r'^[0-9A-F]{4}  ', following) and '\t' not in following:
                    data.add(label.group(1))
        current = None
        for i, line in enumerate(lines):
            label = re.match(r'^[0-9A-F]{4}\t+(\w+):$', line)
            if label:
                current = label.group(1)
                continue
            if '\t' not in line:
                continue
            for name in re.findall(r'\b(_\w+)\b', line.split('\t')[-1]):
                if name in data and (name, current) not in ALLOWED:
                    problems.append(f'{obj}: {current} references data {name}: {line.strip()}')
    for obj, lines in listings.items():
        if 'CVGA' in obj.upper().rsplit('/', 1)[-1]:
            continue                      # the model's own BIOS helpers
        for line in lines:
            if any(name in line for name in BIOS_ONLY) and 'call' in line:
                problems.append(f'{obj}: calls a BIOS-only table user: {line.strip()}')
    if problems:
        print('[build-hdpmi] ERROR: DS-relative data in HDPMI C objects:', file=sys.stderr)
        for problem in problems:
            print('  ' + problem, file=sys.stderr)
        return 1
    print(f'[build-hdpmi] {len(objects)} C objects use no DS-relative data')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
