#!/usr/bin/env python3
"""F5-14B actual-PE import audit and original DirectSetup builtin producer.

Source-reference candidates are never promoted to linked/load evidence. No
network access, Wine implementation substitution, or OS image build occurs.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import argparse
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

WINE_MODULES = ('comctl32', 'comdlg32', 'shell32', 'shlwapi', 'ole32',
                'combase', 'oleaut32', 'rpcrt4', 'riched20', 'riched32',
                'cabinet', 'advpack', 'setupapi', 'msvcrt', 'version')
BUILTINS = ('dsetup.dll', 'dsetup32.dll')
ORIGIN = 'CiukiOS DirectSetup installation identity'
INVENTORY = 'upper-builtins-inventory.json'
MEMORY_MODEL = ('Cold section-store header/raw seed page union; writable COW '
                'commitment promises counted separately, not resident pages. '
                'Excludes DllMain heaps, page tables, metadata and other modules; '
                'not guest RSS or a whole-system 128 MiB fit result.')
FLAGS = ['--target=i686-pc-windows-msvc', '-march=pentiumpro', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-mno-sse', '-mno-sse2',
         '-mno-mmx', '-O1', '-Wall', '-Wextra', '-Werror',
         '-I' + str(ROOT / 'sdk/win32/include')]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def dump(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def run(args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True,
                          text=True, timeout=120)


def memory_model(pe):
    """Match sections.c image_seed and image-page protection union exactly.

    The kernel PE parser uses max(VirtualSize, SizeOfRawData) as its section
    span for protection; overlapping partial pages form a union.
    """
    seed = set(range((pe.headers + 4095) // 4096))
    writable = set()
    for section in pe.sections:
        rva, raw = section['rva'], section['raw_size']
        if raw:
            seed.update(range(rva // 4096, (rva + raw + 4095) // 4096))
        span = section['span']
        if section['flags'] & 0x80000000 and span:
            writable.update(range(rva // 4096, (rva + span + 4095) // 4096))
    return {'shared_seed_pages': len(seed),
            'private_cow_commitment_pages': len(writable),
            'startup_committed_pages_model': len(seed) + len(writable),
            'reserved_image_pages': (pe.size + 4095) // 4096}


def read_images(folder):
    """Only DLL/DRV/ACM providers from this explicit directory, no host Wine."""
    result = {}
    for path in sorted(Path(folder).iterdir()):
        if path.suffix.lower() not in ('.dll', '.drv', '.acm') or not path.is_file():
            continue
        name = path.name.lower()
        if name in result:
            raise ValueError('duplicate PE provider: ' + name)
        pe = census.PE(path.read_bytes())
        metadata = pe.census()
        if not metadata['dll'] or set(metadata['warnings']) - {'newer-subsystem-version'}:
            raise ValueError('invalid DLL provider: ' + name)
        result[name] = (pe, metadata)
    return result


def export_index(metadata):
    result = {}
    for entry in metadata['exports']:
        for symbol in ['#' + str(entry['ordinal']), *entry['names']]:
            if symbol in result:
                raise ValueError('duplicate PE export: ' + symbol)
            result[symbol] = entry
    return result


def resolve(index, dll, symbol, chain=()):
    key = dll.lower() + '!' + symbol
    if key in chain:
        return {'error': 'forwarder-cycle', 'chain': [*chain, key]}
    chain = (*chain, key)
    if dll.lower() not in index:
        return {'error': 'missing-dll', 'chain': list(chain)}
    entry = index[dll.lower()].get(symbol)
    if entry is None:
        return {'error': 'missing-export', 'chain': list(chain)}
    forward = entry['forwarder']
    if not forward:
        return {'error': None, 'chain': list(chain)}
    if len(chain) > 16:
        return {'error': 'forwarder-depth', 'chain': list(chain)}
    # The production loader splits at the first dot and adds .dll.
    target, _, name = forward.partition('.')
    if not target or not name or not re.fullmatch(r'[\w-]+', target):
        return {'error': 'export-forwarder-format', 'chain': list(chain)}
    if name.startswith('#') and not re.fullmatch(r'#[0-9]+', name):
        return {'error': 'export-forwarder-format', 'chain': list(chain)}
    if name.startswith('#') and int(name[1:]) > 65535:
        return {'error': 'export-forwarder-format', 'chain': list(chain)}
    if name.startswith('#'):
        name = '#' + str(int(name[1:]))
    return resolve(index, target + '.dll', name, chain)


def audit_images(folder, names=None):
    images = read_images(folder)
    index = {name: export_index(metadata) for name, (_, metadata) in images.items()}
    result = {}
    for name in sorted(names if names is not None else images):
        if name not in images:
            result[name] = {'error': 'not-built', 'imports': [], 'forwarders': []}
            continue
        pe, metadata = images[name]
        rows = []
        for delayed in (False, True):
            for item in pe.imports(delayed):
                provider = item['dll'].lower()
                required = sorted(set(item['symbols']))
                resolutions = {symbol: resolve(index, provider, symbol) for symbol in required}
                rows.append({'dll': provider, 'delayed': delayed, 'required': required,
                             'present': [s for s in required if not resolutions[s]['error']],
                             'missing': [s for s in required if resolutions[s]['error']],
                             'resolutions': resolutions})
        forwards = []
        for entry in metadata['exports']:
            if entry['forwarder']:
                symbol = '#' + str(entry['ordinal'])
                forwards.append({'ordinal': entry['ordinal'], 'names': entry['names'],
                                 'target': entry['forwarder'],
                                 **resolve(index, name, symbol)})
        result[name] = {'error': None, 'imports': rows, 'forwarders': forwards,
                        'sha256': sha(Path(folder) / next(p.name for p in Path(folder).iterdir()
                                                        if p.name.lower() == name)),
                        'file_bytes': len(pe.data), 'size_of_image': pe.size,
                        'memory': memory_model(pe)}
    return result


def audit_passed(audit):
    return bool(audit) and all(not row['error'] and not any(i['missing'] for i in row['imports'])
               and not any(f['error'] for f in row['forwarders']) for row in audit.values())


def core_inventory(folder):
    """Measured provider names, distinguishing exported W21 remainders."""
    result = {}
    for name, (_, metadata) in read_images(folder).items():
        if name not in ('ntdll.dll', 'kernel32.dll', 'advapi32.dll', 'user32.dll', 'gdi32.dll'):
            continue
        coverage = Path(folder) / (name.removesuffix('.dll') + '-coverage.json')
        status = {}
        if coverage.is_file():
            for row in json.loads(coverage.read_text())['exports']:
                status[row['name']] = row['status']
        result[name.removesuffix('.dll')] = {
            n: status.get(n, 'exported; no per-function coverage classification')
            for e in metadata['exports'] for n in e['names']}
    return result


def source_audit(folder):
    """F5-14A candidates vs actual core, expressly NOT actual PE imports."""
    pin = json.loads(wine_audit.PIN.read_text())
    audit = json.loads(wine_audit.AUDIT.read_text())
    wine_audit.validate(audit, pin)
    providers = core_inventory(folder)
    result = {}
    for module in WINE_MODULES:
        if module not in audit['roots']:
            result[module] = {'status': 'no direct source-reference census; full source required'}
            continue
        rows = {}
        for provider, required in audit['roots'][module]['core_exports'].items():
            available = providers.get(provider, {})
            rows[provider] = {'required': required,
                              'present': {n: available[n] for n in required if n in available},
                              'missing': {n: 'unresolved: requires a reviewed core amendment or implemented Wine-side patch'
                                          for n in required if n not in available}}
        result[module] = {'status': 'source-reference candidates, not linked imports', 'providers': rows}
    return {'wine_commit': pin['commit'], 'modules': result,
            'core_pe_sha256': {name: sha(path) for path in sorted(Path(folder).iterdir())
                              if (name := path.name.lower()) in {p + '.dll' for p in providers}},
            'model': MEMORY_MODEL, 'original_title_census': 'absent; no acceptance-set claims'}


def build(out, base=0x73000000, flags=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    flags = list(flags or FLAGS)
    obj = out / 'dsetup.obj'
    run(['clang', *flags, '-c', ROOT / 'sdk/win32/upper/dsetup.c', '-o', obj])
    rc = out / 'dsetup-version.rc'
    rc.write_text('LANGUAGE 9,1\n1 VERSIONINFO\nFILEVERSION 4,7,0,716\n'
                  'PRODUCTVERSION 4,7,0,716\nFILEFLAGSMASK 0x3f\nFILEFLAGS 0\n'
                  'FILEOS 0x40004\nFILETYPE 2\nBEGIN\nEND\n')
    resource = out / 'dsetup-version.res'
    run(['llvm-rc', '/fo', resource, rc])
    records = []
    for name in BUILTINS:
        definition = out / (name + '.def')
        definition.write_text('LIBRARY ' + name + '\nEXPORTS\n'
                              ' DirectXSetup=_DirectXSetupA@12\n'
                              ' DirectXSetupA=_DirectXSetupA@12\n'
                              ' DirectXSetupW=_DirectXSetupW@12\n'
                              ' DirectXSetupGetVersion=_DirectXSetupGetVersion@8\n')
        image = out / name
        run(['lld-link', '/machine:x86', '/dll', '/entry:CiukiSetupDllMain@12',
             '/nodefaultlib', '/safeseh:no', '/timestamp:0', '/incremental:no',
             '/base:' + hex(base), '/subsystem:windows,4.0', '/osversion:4.0',
             '/def:' + str(definition), '/out:' + str(image), obj, resource])
        raw = image.read_bytes()
        pe = census.PE(raw)
        records.append({'name': name, 'path': name, 'base': base, 'size': pe.size,
                        'sha256': sha(image), 'version': stubs.image_version(raw),
                        'license': 'LGPL-2.1-or-later', 'origin': ORIGIN,
                        'wine_reused': False, 'file_bytes': len(raw),
                        'source_sha256': sha(ROOT / 'sdk/win32/upper/dsetup.c'),
                        'memory': memory_model(pe)})
        base += (pe.size + 65535) & ~65535
    audit = audit_images(out, BUILTINS)
    if not audit_passed(audit):
        raise ValueError('DirectSetup unresolved imports or forwards')
    dump(out / INVENTORY, {'schema': 1, 'dlls': records, 'imports': audit,
                           'model': MEMORY_MODEL,
                           'scope': 'DirectSetup identity only; Wine upper DLLs remain unqualified'})
    return records


def verify_builtins(folder, store):
    folder = Path(folder)
    records = [d for d in store['dlls'] if d.get('origin') == ORIGIN]
    if not records and not (folder / INVENTORY).exists():
        return
    inventory = json.loads((folder / INVENTORY).read_text())
    if (inventory['schema'] != 1 or {d['name'] for d in records} != set(BUILTINS)
            or inventory['dlls'] != records or inventory['model'] != MEMORY_MODEL):
        raise ValueError('upper builtin inventory mismatch')
    for record in records:
        pe = census.PE((folder / record['path']).read_bytes())
        if (record.get('wine_reused') is not False or record.get('license') != 'LGPL-2.1-or-later'
                or record.get('source_sha256') != sha(ROOT / 'sdk/win32/upper/dsetup.c')
                or record.get('file_bytes') != len(pe.data) or record.get('memory') != memory_model(pe)):
            raise ValueError('upper builtin license/source/memory mismatch')
    actual = audit_images(folder, BUILTINS)
    if inventory['imports'] != actual or not audit_passed(actual):
        raise ValueError('upper builtin import audit mismatch')


def reject_unqualified_wine(folder, store):
    """No reused P4 PE may enter an image before B qualification exists.

    Also reject additional PEs in a staged directory which are absent from its
    store manifest. Packaging must not silently ignore a copied upper DLL.
    """
    expected = {d['name'].lower() for d in store['dlls']}
    for path in Path(folder).iterdir():
        if path.is_file() and path.suffix.lower() in ('.dll', '.drv', '.acm'):
            if path.name.lower() not in expected:
                raise ValueError('unlisted builtin PE: ' + path.name)
    for record in store['dlls']:
        if record.get('wine_reused') is True:
            raise ValueError('unqualified reused Wine PE: ' + record['name'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    sub = commands.add_parser('build-builtins')
    sub.add_argument('output', type=Path)
    for name in ('audit', 'source-audit'):
        sub = commands.add_parser(name)
        sub.add_argument('directory', type=Path)
        sub.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.command == 'build-builtins':
        print(json.dumps({r['name']: r['sha256'] for r in build(args.output)}, indent=2))
    elif args.command == 'source-audit':
        dump(args.output, source_audit(args.directory))
    else:
        evidence = audit_images(args.directory)
        dump(args.output, evidence)
        if not audit_passed(evidence):
            raise SystemExit('unresolved PE imports/forwarders; see ' + str(args.output))


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.stdout or '')
        sys.stderr.write(exc.stderr or '')
        sys.exit(exc.returncode)
