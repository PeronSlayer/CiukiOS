#!/usr/bin/env python3
"""Deterministic P5 i386 PE builds and exact import/coverage/size inventories."""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import census, stubs, wine_audit

MODULES = ('msacm32', 'imaadp32.acm', 'msadp32.acm', 'msvfw32',
           'avifil32', 'msrle32', 'msvidc32', 'iccvid')
FLAGS = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-mno-sse', '-mno-sse2', '-mno-mmx',
         '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'sdk/win32/include')]
KERNEL = {'GetProcessHeap': 0, 'HeapAlloc': 12, 'HeapFree': 12,
          'CreateFileA': 28, 'WideCharToMultiByte': 32, 'GetFileSize': 8,
          'SetFilePointer': 16, 'ReadFile': 20, 'CloseHandle': 4}
VCM = {'ICLocate': 20, 'ICSendMessage': 16, 'ICClose': 4, 'ICDecompress': None}


def run(args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_sources():
    folder = ROOT/'third_party/wine'
    record = json.loads((folder/'codecs-sources.json').read_text())
    if record['commit'] != json.loads((ROOT/'config/wine-pin.json').read_text())['commit']:
        raise ValueError('codec Wine commit mismatch')
    for name, digest in record['files'].items():
        if sha(folder/name) != digest:
            raise ValueError('codec interface source hash: '+name)


def import_library(out, name, entries):
    definition = out/(name+'-codec-imports.def')
    definition.write_text('LIBRARY '+name+'.dll\nEXPORTS\n'+''.join(
        ' '+n+(('@'+str(a)) if a is not None else '')+'\n' for n, a in entries.items()))
    library = out/(name+'-codec-imports.lib')
    run(['llvm-dlltool', '-m', 'i386', '-k', '-d', definition, '-l', library])
    return library


def unsupported_return(module, name):
    if module == 'msacm32':
        return '8'
    if module == 'avifil32':
        return '0' if name in ('AVIStreamGetFrame', 'AVIStreamGetFrameOpen', 'AVISaveOptions') else ('-1' if name == 'AVIStreamFindSample' else '(int32_t)0x80044065u')
    return '0' if name.startswith(('DrawDib', 'MCIWnd', 'ICOpen', 'ICImage', 'ICSeq', 'ICCompressor', 'GetOpen', 'GetSave', 'ICGetDisplay')) else '-1'


def audit_imports(out, records):
    """Use actual exported names, including peer P5 modules; never source guesses."""
    providers = {}
    for image in sorted(Path(out).glob('*.dll')):
        providers[image.name.lower()] = {n for e in census.PE(image.read_bytes()).exports() for n in e['names']}
    result = {}
    for record in records:
        image = census.PE((Path(out)/record['path']).read_bytes())
        entries = []
        for imported in image.imports()+image.imports(delayed=True):
            provider = imported['dll'].lower()
            available = providers.get(provider, set())
            entries.append({'dll': provider, 'required': imported['symbols'],
                            'present': sorted(set(imported['symbols']) & available),
                            'missing': sorted(set(imported['symbols']) - available)})
        result[record['name']] = entries
    return result


def build(out, base=0x72000000, flags=None):
    verify_sources()
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    flags = list(flags or FLAGS)
    notices = {n: sha(ROOT/'third_party/wine'/n) for n in ('LICENSE','COPYING.LIB','AUTHORS','SOURCE-OFFER.txt','PATCHES.md')}
    interface_sha = sha(ROOT/'third_party/wine/codecs-sources.json')
    kernel = import_library(out, 'kernel32', KERNEL)
    vcm = import_library(out, 'msvfw32', VCM)
    result = []
    for module in MODULES:
        name = module if module.endswith('.acm') else module+'.dll'
        folder = ROOT/'sdk/win32/codecs'
        if module in ('msacm32', 'imaadp32.acm', 'msadp32.acm'):
            units = ['adpcm.c', 'acm.c']
            defines = [] if module == 'msacm32' else ['-DCC_AUDIO_DRIVER='+('17' if module.startswith('ima') else '2')]
        elif module == 'avifil32':
            units = ['avi.c']; defines = []
        else:
            units = ['video.c', 'vcm.c']; defines = [] if module == 'msvfw32' else ['-DCC_VIDEO_DRIVER='+str({'msrle32': 0x656c726d, 'msvidc32': 0x6376736d, 'iccvid': 0x64697663}[module])]
        objects = []; symbols = {}
        for unit in units:
            obj = out/(module+'-'+unit+'.obj')
            run(['clang', *flags, *defines, '-c', folder/unit, '-o', obj]); objects.append(obj)
            nm = run(['llvm-nm', '--defined-only', obj]).stdout
            for decoration in re.findall(r'\bT\s+(_[A-Za-z][\w@]*)', nm):
                symbols[decoration[1:].split('@')[0]] = decoration
        specs = wine_audit.parse_spec((ROOT/'third_party/wine/codecs'/module/(module+'.spec')).read_text())
        exports = []; fallback = ['#include <stdint.h>']; coverage = {}
        for i, (export, spec) in enumerate(sorted(specs.items())):
            # Source aliases of public A/W APIs retain the same PE implementation.
            original_line = next((l for l in (ROOT/'third_party/wine/codecs'/module/(module+'.spec')).read_text().splitlines() if re.search(r'\b'+re.escape(export)+r'\s*\(', l)), '')
            alias = re.search(r'\)\s+(\w+)', original_line)
            target = export if export in symbols else (alias[1] if alias and alias[1] in symbols else export)
            if target in symbols:
                decoration = symbols[target]; coverage[export] = 'implemented'
            elif spec['kind'] == 'extern':
                guid = {'CLSID_AVISimpleUnMarshal': 0x20009, 'IID_IAVIFile': 0x20020,
                        'IID_IAVIStream': 0x20021, 'IID_IGetFrame': 0x20023, 'IID_IAVIEditStream': 0x20024}[export]
                fallback.append(f'const uint32_t data{i}[4]={{{guid},0,0xc0,0x46000000}};')
                decoration = '_data'+str(i); coverage[export] = 'constant'
            else:
                args = re.search(r'\(([^)]*)\)', original_line)
                count = len(args[1].split()) if args else 0
                cc = '__attribute__((stdcall)) ' if spec['kind'] == 'stdcall' else ''
                params = ','.join(f'uintptr_t a{n}' for n in range(count)) or 'void'
                fallback.append(f'int32_t {cc}unsupported{i}({params})'+'{'+''.join(f'(void)a{n};' for n in range(count))+'return '+unsupported_return(module, export)+';}')
                decoration = f'_unsupported{i}'+('@'+str(count*4) if cc else '')
                coverage[export] = 'unsupported; clean error'
            if '@' not in decoration:
                decoration = decoration.removeprefix('_')
            suffix = ' @'+str(spec['ordinal']) if spec['ordinal'] is not None else ''
            if spec['kind'] == 'extern': suffix += ' DATA'
            exports.append(f' {export}={decoration}{suffix}\n')
        remainder = out/(module+'-unsupported.c'); remainder.write_text('\n'.join(fallback)+'\n')
        obj = out/(module+'-unsupported.obj'); run(['clang', *flags, '-c', remainder, '-o', obj]); objects.append(obj)
        definition = out/(name+'.def'); definition.write_text('LIBRARY '+name+'\nEXPORTS\n'+''.join(exports))
        rc = out/(module+'-version.rc');rc.write_text('LANGUAGE 9,1\n1 VERSIONINFO\nFILEVERSION 1,0,0,0\nPRODUCTVERSION 1,0,0,0\nFILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nBEGIN\nEND\n')
        res = out/(module+'-version.res');run(['llvm-rc','/fo',res,rc])
        image = out/name
        run(['lld-link','/machine:x86','/dll','/noentry','/nodefaultlib','/safeseh:no',
             '/timestamp:0','/incremental:no','/base:'+hex(base),'/subsystem:windows,4.0',
             '/osversion:4.0','/def:'+str(definition),'/out:'+str(image),*objects,res,kernel,
             *([vcm] if module=='avifil32' else [])])
        pe = census.PE(image.read_bytes())
        # sections.c image_seed allocates header/raw backing pages; map_view
        # preallocates COW credits for every writable image page. BSS backing
        # itself remains demand-created. Metadata/page tables are excluded.
        seeded=set(range((pe.headers+4095)//4096)); writable=set()
        for section in pe.sections:
            rva=section['rva']; raw=section['raw_size']; span=section['span']
            if raw: seeded.update(range(rva//4096,(rva+raw+4095)//4096))
            if section['flags']&0x80000000 and span:
                writable.update(range(rva//4096,(rva+span+4095)//4096))
        record = {'name':name,'path':name,'base':base,'size':pe.size,
                  'sha256':sha(image),'version':stubs.image_version(image.read_bytes()),
                  'license':'LGPL-2.1-or-later','origin':'CiukiOS bounded P5 implementation',
                  'wine_reused':False,'notices':notices,'interface_sha256':interface_sha,'file_bytes':image.stat().st_size,
                  'resident_pages_store_model':len(seeded)+len(writable),
                  'shared_seed_pages':len(seeded),'private_cow_credit_pages':len(writable),
                  'reserved_image_pages':(pe.size+4095)//4096,
                  'coverage':coverage}
        result.append(record);base+=(pe.size+65535)&~65535
    evidence = {'schema':1,'dlls':result,'imports':audit_imports(out,result),
                'wine_commit':'b073859675060c9211fcbccfd90e4e87520dc2c2',
                'model':'Cold sections.c image_seed header/raw page union plus map_view writable COW credits; excludes metadata/page tables; not guest RSS'}
    (out/'codecs-inventory.json').write_text(json.dumps(evidence,sort_keys=True,indent=2)+'\n')
    return result


if __name__ == '__main__':
    try:
        out = Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'build/host/win32/codecs/pe'
        records = build(out)
        print(json.dumps({r['name']:r['sha256'] for r in records},indent=2))
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.stdout or '');sys.stderr.write(exc.stderr or '');sys.exit(exc.returncode)
