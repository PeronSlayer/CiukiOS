#!/usr/bin/env python3
"""Offline NSIS 2.51 host compiler, using the lead's two verified archives.

Only makensis is compiled. The release ZIP supplies the exact Windows stubs,
includes and UI resources; no Windows compiler or system NSIS is used. The
Python 2 SCons driver is replaced by its Source/SConscript translation-unit
list and SCons/config.py defaults (recorded in build.json).
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / 'config/nsis.json'
OUT = ROOT / 'build/tools/nsis'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def archives():
    pin = json.loads(PIN.read_text())
    result = {}
    for kind in ('source', 'binary'):
        entry = pin[kind]
        path = ROOT / entry['file']
        if not path.is_file():
            raise FileNotFoundError(f'NSIS {kind} archive missing: {path}; no download or fallback')
        if path.stat().st_size != entry['size'] or sha(path) != entry['sha256']:
            raise ValueError(f'NSIS {kind} archive does not match config/nsis.json')
        result[kind] = path
    return pin, result


def configuration(source):
    # Read precisely the upstream default declarations, without executing the
    # old build driver. Quoted descriptions can contain escaped quotes.
    text = (source / 'SCons/config.py').read_text()
    values = {}
    for name, default in re.findall(
            r"'((?:NSIS_)[A-Z_0-9]+)',\s*(?:'[^'\\]*(?:\\.[^'\\]*)*'|\"[^\"\\]*(?:\\.[^\"\\]*)*\"),\s*([^\n]+)", text):
        default = default.strip().rstrip(',')
        if default in ("'yes'", "'no'"):
            values[name] = default == "'yes'"
        elif default.isdigit():
            values[name] = int(default)
        elif default.startswith("'."):
            values[name] = default.strip("'")
        elif name == 'NSIS_CONFIG_CONST_DATA_PATH':
            values[name] = False  # relocatable, NSISDIR is explicit at use
        else:
            raise ValueError(f'unrecognised NSIS default {name}={default}')
    expected = set(re.findall(r"Add(?:Bool|Valued|String)Define\('([^']+)'\)", text))
    if set(values) != expected:
        raise ValueError(f'unparsed NSIS defaults: {expected - set(values)}')
    return values


def build(out=OUT):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    pin, files = archives()
    key = {k: pin[k]['sha256'] for k in files}
    key['recipe'] = sha(__file__)
    compiler = subprocess.run(['g++', '--version'], check=True, capture_output=True, text=True).stdout.splitlines()[0]
    key['compiler'] = compiler
    record = out / 'build.json'
    program = out / 'makensis'
    if record.is_file() and program.is_file():
        old = json.loads(record.read_text())
        if old['inputs'] == key and old['makensis_sha256'] == sha(program):
            return program
    source, data, objects = out / 'source', out / 'data', out / 'objects'
    for folder in (source, data, objects):
        folder.mkdir(exist_ok=True)
    with tarfile.open(files['source']) as archive:
        archive.extractall(source, filter='data')
    source /= 'nsis-2.51-src'
    with zipfile.ZipFile(files['binary']) as archive:
        for entry in archive.infolist():
            if entry.filename.startswith('/') or '..' in Path(entry.filename).parts:
                raise ValueError('unsafe NSIS ZIP member')
        archive.extractall(data)
    data /= 'nsis-2.51'
    # The 2015 host recipe requests -m32. The sandbox refuses i386 ELF
    # startup, so use native pointers for host-only pointer arithmetic while
    # retaining 32-bit on-disk Windows types. These exact replacements are
    # recorded; none touches the ZIP's Windows stubs or bytecode engine.
    replacements = {
        'Platform.h': [('typedef unsigned int UINT_PTR;', 'typedef unsigned long UINT_PTR;'),
                       ('typedef long LONG;', 'typedef int LONG;'),
                       ('#  define ULONG_PTR DWORD', '#  define ULONG_PTR uintptr_t'),
                       ('((LONG)&(((t*)0)->f))', '((uintptr_t)&(((t*)0)->f))'),
                       ('(DWORD) h +', '(ULONG_PTR) h +')],
        'ResourceEditor.cpp': [('(WORD) (DWORD) szName', '(WORD) (ULONG_PTR) szName'),
                               ('DWORD(seeker)', 'ULONG_PTR(seeker)'),
                               ('DWORD(pbRsrcSec)', 'ULONG_PTR(pbRsrcSec)'),
                               ('ConvertEndianness(seeker - pbRsrcSec + m_dwResourceSectionVA)',
                                'ConvertEndianness(DWORD(seeker - pbRsrcSec + m_dwResourceSectionVA))'),
                               ('DWORD newResDirAt', 'ULONG_PTR newResDirAt')],
        'ResourceEditor.h': [('DWORD m_dwWrittenAt;', 'ULONG_PTR m_dwWrittenAt;'),
                             ('DWORD newResDirAt', 'ULONG_PTR newResDirAt')],
        'DialogTemplate.cpp': [('(DWORD) seeker - (DWORD) pbDlg', '(ULONG_PTR) seeker - (ULONG_PTR) pbDlg'),
                               ('WORD(DWORD(x))', 'WORD(ULONG_PTR(x))')],
        'mmap.cpp': [('(unsigned int)pView', '(uintptr_t)pView')],
        'Plugins.cpp': [('unsigned long *names = (unsigned long*)', 'DWORD *names = (DWORD*)')],
    }
    for name, changes in replacements.items():
        path = source / 'Source' / name
        text = path.read_text()
        for before, after in changes:
            if before not in text:
                raise ValueError(f'NSIS host adaptation no longer applies: {name}: {before}')
            text = text.replace(before, after)
        path.write_text(text)
    values = configuration(source)
    config = objects / 'nsis-sconf.h'
    config.write_text(''.join(f'#define {n}' + ('' if v is True else ' ' + json.dumps(v)) + '\n'
                             for n, v in values.items() if v is not False))
    (objects / 'nsis-defines.h').write_text(''.join(
        f'definedlist.add("{n}"' + ('' if v is True else ', ' + json.dumps(str(v))) + ');\n'
        for n, v in values.items() if v is not False))
    (objects / 'nsis-version.h').write_text('#define NSIS_VERSION "v2.51"\n')
    script = (source / 'Source/SConscript').read_text()
    units = re.findall(r'\b[\w/]+\.(?:cpp|c)\b', script)
    flags = ['-O2', '-fno-strict-aliasing', '-D_FILE_OFFSET_BITS=64', '-D_WIN32_IE=0x0500', '-DNSISCALL=',
             '-I' + str(objects), '-I' + str(source / 'Source')]
    built = []
    for i, unit in enumerate(units):
        obj = objects / f'{i}.o'
        command = ['g++', '-std=gnu++98'] if unit.endswith('.cpp') else ['gcc']
        if unit.endswith('.cpp'):
            command += ['-include', 'unistd.h', '-include', 'stdint.h']
        subprocess.run([*command, *flags, *(['-DCOMPRESS_MF_BT'] if unit.startswith('7zip/') else []),
                        '-c', str(source / 'Source' / unit), '-o', str(obj)], check=True)
        built.append(obj)
    subprocess.run(['g++', '-o', str(program), *map(str, built), '-pthread'], check=True)
    version = subprocess.run([program, '-VERSION'], env=dict(os.environ, NSISDIR=str(data)),
                             check=True, capture_output=True, text=True).stdout.strip()
    if version != 'v2.51':
        raise ValueError(f'wrong compiler version: {version}')
    record.write_text(json.dumps({'inputs': key, 'defaults': values, 'version': version,
                                 'host_adaptations': replacements,
                                 'makensis_sha256': sha(program), 'data': str(data)}, indent=2) + '\n')
    return program


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT)
    args = parser.parse_args()
    print(build(args.output))
