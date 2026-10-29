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
# Reused Wine PE build record (f5-14B continuation): hashes, bases, licence.
UPPER_RECORD = ROOT / 'config/wine-upper.json'
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
    """Only DLL/DRV/ACM/OCX providers from this explicit directory, no host Wine."""
    result = {}
    for path in sorted(Path(folder).iterdir()):
        if path.suffix.lower() not in ('.dll', '.drv', '.acm', '.ocx') or not path.is_file():
            continue
        name = path.name.lower()
        if name in result:
            raise ValueError('duplicate PE provider: ' + name)
        pe = census.PE(path.read_bytes())
        # Import qualification needs headers/exports, not a resource walk.
        # Large Wine translation tables can exceed the title census budget;
        # this is deliberately not a replacement for the image validator.
        metadata = {'dll': bool(pe.flags & 0x2000), 'warnings': pe.warnings,
                    'exports': pe.exports()}
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


# f5-20C: CiukiOS W21 stub DLLs for the DLLs the reused Wine DLLs delay-load
# but Windows 98 and CiukiOS lack (config/wine-upper.json rule
# absent-delayed-dll). Delay binding succeeds; every export logs one W21
# diagnostic (ntdll w32_shim_stub, last error 120) and returns the failure the
# Microsoft documentation gives for the function. No export reports success.
STUB_ORIGIN = 'CiukiOS W21 stub for an absent delay-loaded DLL'
STUB_INVENTORY = 'absent-dll-stubs-inventory.json'
STUB_FAILURES = {
    'E_NOTIMPL': '0x80004001u', 'E_FAIL': '0x80004005u', 'NULL': '0u', 'FALSE': '0u',
    'SEC_E_UNSUPPORTED_FUNCTION': '0x80090302u', 'SEC_E_SECPKG_NOT_FOUND': '0x80090305u',
    'GpStatus NotImplemented': '6u', 'ERROR_NOT_SUPPORTED': '50u'}
_SSPI = {n: 'SEC_E_UNSUPPORTED_FUNCTION' for n in (
    'AcceptSecurityContext', 'CompleteAuthToken', 'DecryptMessage', 'DeleteSecurityContext',
    'EncryptMessage', 'EnumerateSecurityPackagesA', 'EnumerateSecurityPackagesW',
    'FreeContextBuffer', 'FreeCredentialsHandle', 'ImpersonateSecurityContext',
    'InitializeSecurityContextW', 'MakeSignature', 'QueryContextAttributesA',
    'RevertSecurityContext', 'VerifySignature')}
ABSENT_DELAYED = {
    'gdiplus.dll': {n: 'GpStatus NotImplemented' for n in (
        'GdipCreateFromHDC', 'GdipDeleteGraphics', 'GdipDisposeImage', 'GdipDrawImageRectRectI',
        'GdipGetImageDimension', 'GdipGetImageThumbnail', 'GdipLoadImageFromFile')},
    'iphlpapi.dll': {'GetAdaptersInfo': 'ERROR_NOT_SUPPORTED',
                    'GetAdaptersAddresses': 'ERROR_NOT_SUPPORTED'},
    'crypt32.dll': {**{n:'NULL' for n in ('CertCreateCertificateContext', 'CertDuplicateCertificateChain')},
                    **{n:'FALSE' for n in ('CertFreeCertificateChain', 'CertFreeCertificateContext', 'CertNameToStrA')}},
    'cryptui.dll': {'CryptUIDlgViewCertificateW':'FALSE'},
    'dhcpcsvc.dll': {'DhcpRequestParams':'ERROR_NOT_SUPPORTED'},
    'mlang.dll': {'ConvertINetUnicodeToMultiByte': 'E_FAIL'},
    'secur32.dll': {**_SSPI, 'AcquireCredentialsHandleA': 'SEC_E_SECPKG_NOT_FOUND',
                    'AcquireCredentialsHandleW': 'SEC_E_SECPKG_NOT_FOUND',
                    'GetUserNameExW': 'FALSE',
                    'QueryContextAttributesW':'SEC_E_SECPKG_NOT_FOUND',
                    'QuerySecurityPackageInfoW':'SEC_E_SECPKG_NOT_FOUND'},
    # SHELL32 binds ordinal 218 (IEParseDisplayNameWithBCW, NONAME upstream).
    'shdocvw.dll': {'#218': 'E_NOTIMPL'},
    'urlmon.dll': {n: 'E_NOTIMPL' for n in ('CoInternetParseUrl', 'CreateFormatEnumerator',
                                            'CreateURLMoniker', 'RegisterFormatEnumerator')},
    'uxtheme.dll': {**{n: 'E_NOTIMPL' for n in (
        'CloseThemeData', 'DrawThemeBackground', 'DrawThemeEdge', 'DrawThemeParentBackground',
        'DrawThemeText', 'DrawThemeTextEx', 'EnableThemeDialogTexture',
        'GetThemeBackgroundContentRect', 'GetThemeBackgroundExtent', 'GetThemeColor',
        'GetThemeFont', 'GetThemeInt', 'GetThemeMargins', 'GetThemePartSize',
        'GetThemeTextExtent')},
        'GetWindowTheme': 'NULL', 'OpenThemeData': 'NULL', 'OpenThemeDataForDpi': 'NULL',
        'IsThemeBackgroundPartiallyTransparent': 'FALSE', 'IsThemePartDefined': 'FALSE'},
    'windowscodecs.dll': {'WICConvertBitmapSource': 'E_NOTIMPL'},
    'wintrust.dll': {'CryptCATAdminAcquireContext': 'FALSE', 'CryptCATAdminAddCatalog': 'NULL',
                     'CryptCATAdminReleaseCatalogContext': 'FALSE',
                     'CryptCATAdminReleaseContext': 'FALSE'},
}
# Upstream names of ordinal-only exports (pinned Wine spec snapshots).
STUB_ORDINAL_NAMES = {('shdocvw.dll', 218): 'IEParseDisplayNameWithBCW'}
STUB_GENERATOR = Path(__file__).resolve()


def absent_stub_sources(dll):
    """C source, .def and version script of one absent-DLL W21 stub."""
    from scripts.win32 import stub_signatures
    c = ['/* SPDX-License-Identifier: LGPL-2.1-or-later */',
         '/* Copyright 2026 CiukiOS contributors. Generated by scripts/win32/upper_dlls.py. */',
         '__declspec(dllimport) unsigned w32_shim_stub(volatile unsigned *, const char *, const char *, unsigned);']
    definition = ['LIBRARY ' + dll, 'EXPORTS']
    exports = []
    for index, (symbol, failure) in enumerate(sorted(ABSENT_DELAYED[dll].items())):
        ordinal = int(symbol[1:]) if symbol.startswith('#') else None
        name = STUB_ORDINAL_NAMES[(dll, ordinal)] if ordinal is not None else symbol
        sig = stub_signatures.signature(dll, name)
        if not sig or sig['calling'] != 'stdcall':
            raise ValueError('no stdcall signature for ' + dll + '!' + name)
        words = sum(sig['widths']) // 4
        args = ', '.join(f'unsigned a{i}' for i in range(words)) or 'void'
        unused = ''.join(f'(void)a{i}; ' for i in range(words))
        c.append(f'static volatile unsigned once_{index};\n'
                 f'unsigned __attribute__((stdcall)) w21_{index:02d}({args}) {{ {unused}'
                 f'(void)w32_shim_stub(&once_{index}, "{dll}", "{name}", 0); '
                 f'return {STUB_FAILURES[failure]}; }}')
        target = f'_w21_{index:02d}@{words * 4}'
        definition.append(f'    W32_Ordinal_{ordinal}={target} @{ordinal} NONAME' if ordinal is not None
                          else f'    {name}={target}')
        exports.append({'symbol': symbol, 'name': name, 'failure': failure,
                        'stack_bytes': words * 4, 'signature': sig['source']})
    rc = ('LANGUAGE 9, 1\n1 VERSIONINFO\nFILEVERSION 0,0,0,0\nPRODUCTVERSION 0,0,0,0\n'
          'FILEFLAGSMASK 0x3f\nFILEFLAGS 0\nFILEOS 0x40004\nFILETYPE 2\nFILESUBTYPE 0\n'
          'BEGIN\n BLOCK "StringFileInfo"\n BEGIN\n  BLOCK "040904E4"\n  BEGIN\n'
          f'   VALUE "FileDescription", "CiukiOS W21 stub {dll}\\0"\n'
          '   VALUE "FileVersion", "0.0.0.0\\0"\n   VALUE "ProductName", "CiukiOS\\0"\n'
          '  END\n END\n BLOCK "VarFileInfo"\n BEGIN\n  VALUE "Translation", 0x409, 1252\n END\nEND\n')
    return '\n'.join(c) + '\n', '\n'.join(definition) + '\n', rc, exports


def build_absent_stubs(out, base, flags=None):
    """The W21 stub DLLs of ABSENT_DELAYED at consecutive 64 KiB bases."""
    out = Path(out)
    work = out / 'absent-stubs'
    work.mkdir(parents=True, exist_ok=True)
    flags = list(flags or FLAGS)
    imports = work / 'ntdll-w21.def'
    imports.write_text('LIBRARY ntdll.dll\nEXPORTS\n    w32_shim_stub\n')
    library = work / 'ntdll-w21.lib'
    run(['llvm-dlltool', '-m', 'i386', '-d', imports, '-l', library])
    records = []
    for dll in sorted(ABSENT_DELAYED):
        stem = dll.removesuffix('.dll')
        source, definition, rc, exports = absent_stub_sources(dll)
        (work / (stem + '.c')).write_text(source)
        (work / (stem + '.def')).write_text(definition)
        (work / (stem + '.rc')).write_text(rc)
        run(['clang', *flags, '-c', work / (stem + '.c'), '-o', work / (stem + '.obj')])
        run(['llvm-rc', '/fo', work / (stem + '.res'), work / (stem + '.rc')])
        image = out / dll
        run(['lld-link', '/machine:x86', '/dll', '/noentry', '/nodefaultlib', '/safeseh:no',
             '/timestamp:0', '/incremental:no', '/base:' + hex(base), '/subsystem:windows,4.0',
             '/osversion:4.0', '/def:' + str(work / (stem + '.def')), '/out:' + str(image),
             work / (stem + '.obj'), work / (stem + '.res'), library])
        raw = image.read_bytes()
        pe = census.PE(raw)
        records.append({'name': dll, 'path': dll, 'base': base, 'size': pe.size,
                        'sha256': sha(image), 'version': stubs.image_version(raw),
                        'license': 'LGPL-2.1-or-later', 'origin': STUB_ORIGIN,
                        'wine_reused': False, 'file_bytes': len(raw),
                        'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                        'memory': memory_model(pe)})
        base += (pe.size + 65535) & ~65535
    dump(out / STUB_INVENTORY, {'schema': 1, 'dlls': records, 'model': MEMORY_MODEL,
                                'exports': {dll: absent_stub_sources(dll)[3] for dll in sorted(ABSENT_DELAYED)},
                                'scope': 'W21 failures for absent delay-loaded DLLs of the reused Wine DLLs'})
    return records


# The cached upper build of make build-full (make upper; scripts/win32/wine_upper.py cached-build).
UPPER_PE = ROOT / 'build/win32/upper/pe'
STAGED_ORIGIN = 'Wine 10.0 reused upper DLL (LGPL-2.1-or-later)'


def pe_version(raw):
    """FILEVERSION of VS_VERSION_INFO #1 in its first language (Wine
    resources are language-neutral, unlike the CiukiOS builtins' 1033);
    [0, 0, 0, 0] for a PE without resources or without a version resource
    (ADVPACK, COMBASE, COML2, SHCORE, SETUPAPI), as for the core builtins."""
    pe = census.PE(raw)
    rva, size = pe.dirs[2]
    if not rva or not size:
        return [0, 0, 0, 0]
    def entry(offset, key=None):
        header = pe.read(rva + offset, 16)
        count = int.from_bytes(header[12:14], 'little') + int.from_bytes(header[14:16], 'little')
        for i in range(count):
            e = pe.read(rva + offset + 16 + 8 * i, 8)
            if key is None or int.from_bytes(e[:4], 'little') == key:
                return int.from_bytes(e[4:], 'little') & 0x7fffffff
        return None
    kind = entry(0, 16)
    name = entry(kind, 1) if kind is not None else None
    leaf = entry(name) if name is not None else None
    if leaf is None:
        return [0, 0, 0, 0]
    data = pe.read(rva + leaf, 16)
    blob = pe.read(int.from_bytes(data[:4], 'little'), int.from_bytes(data[4:8], 'little'))
    at = blob.find((0xfeef04bd).to_bytes(4, 'little'))
    if at < 0 or len(blob) < at + 16:
        raise ValueError('reused PE fixed version missing')
    ms, ls = int.from_bytes(blob[at + 8:at + 12], 'little'), int.from_bytes(blob[at + 12:at + 16], 'little')
    return [ms >> 16, ms & 65535, ls >> 16, ls & 65535]


def census_placeholder(folder, entry):
    """True when a store entry is the generated census W21 placeholder of its
    name (byte-identical to the generator's output), which a qualified reused
    Wine PE of the same name replaces in the image."""
    generated = Path(folder) / 'generated' / entry['name']
    return generated.is_file() and sha(generated) == entry['sha256']


def stage_reused(folder, store, pe_dir=None):
    """The qualified reused Wine PEs of config/wine-upper.json added to a
    builtin store for the image: verified copies, manifest and BUILTIN.BIN
    under FOLDER/staged. A census W21 placeholder of the same name (COMCTL32,
    COMDLG32, OLE32, SHELL32) is replaced; any other same-named builtin is
    refused. Returns (store, None) when nothing is qualified."""
    import shutil
    import struct
    record = json.loads(UPPER_RECORD.read_text())
    qualified = {n: r for n, r in sorted(record['dlls'].items()) if r['qualified'] is True}
    if not qualified:
        return store, None
    pe_dir = Path(pe_dir or UPPER_PE)
    out = Path(folder) / 'staged'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()
    dlls, replaced = [], {}
    for entry in store['dlls']:
        name = entry['name'].lower()
        if name in qualified:
            if not census_placeholder(folder, entry):
                raise ValueError('staged reused Wine PE shadows a builtin: ' + name)
            replaced[name] = entry['sha256']
            continue
        dlls.append(entry)
    for name, entry in qualified.items():
        source = pe_dir / name
        if not source.is_file() or sha(source) != entry['sha256']:
            raise ValueError('qualified reused Wine PE missing or changed: ' + name +
                             ' (run make upper: scripts/win32/wine_upper.py cached-build)')
        shutil.copyfile(source, out / name)
        raw = (out / name).read_bytes()
        row = {'name': name, 'path': name, 'base': entry['base'], 'size': entry['size_of_image'],
               'sha256': entry['sha256'], 'version': pe_version(raw),
               'license': entry['license'], 'wine_reused': True, 'origin': STAGED_ORIGIN}
        if name in replaced:
            row['replaces_census_placeholder_sha256'] = replaced[name]
        dlls.append(row)
    spans = sorted((d['base'], d['base'] + d['size'], d['name']) for d in dlls)
    for (start, end, name), following in zip(spans, spans[1:]):
        if end > following[0]:
            raise ValueError('staged builtin overlap: ' + name)
    staged = dict(store, dlls=dlls, msvcrt_policy=(
        'msvcrt.dll: Wine 10.0 PE staged as the builtin MSVCRT (config/wine-upper.json, qualified by '
        'the recorded H0 binding and H1 DllMain evidence; f5-20D cached upper build)'
        if 'msvcrt.dll' in qualified else store.get('msvcrt_policy')))
    reject_unqualified_wine(out, {'dlls': [d for d in dlls if d.get('wine_reused') is True]})
    dump(out / 'builtin-store.json', staged)
    binary = struct.pack('<II', 1, len(dlls))
    for d in dlls:
        binary += struct.pack('<64s65s3xII4H', d['name'].encode(), d['sha256'].encode(), d['base'], d['size'],
                              *d['version'])
    (out / 'BUILTIN.BIN').write_bytes(binary)
    return staged, out


def census_stub_amendments():
    """Exports the reused Wine DLLs need from existing census W21 DLLs
    (config/wine-upper.json rule ciukios-stub-dll): DLL -> names."""
    result = {}
    if UPPER_RECORD.is_file():
        for entry in json.loads(UPPER_RECORD.read_text())['decisions']:
            if entry['rule'] == 'ciukios-stub-dll' and entry['owner'] not in ABSENT_DELAYED:
                result.setdefault(entry['owner'], set()).add(entry['name'])
    return {dll: sorted(names) for dll, names in sorted(result.items())}


def verify_absent_stubs(folder, store):
    folder = Path(folder)
    records = [d for d in store['dlls'] if d.get('origin') == STUB_ORIGIN]
    if not records and not (folder / STUB_INVENTORY).exists():
        return
    inventory = json.loads((folder / STUB_INVENTORY).read_text())
    if (inventory['schema'] != 1 or [d['name'] for d in records] != sorted(ABSENT_DELAYED)
            or inventory['dlls'] != records or inventory['model'] != MEMORY_MODEL):
        raise ValueError('absent-DLL stub inventory mismatch')
    for record in records:
        source, _, _, exports = absent_stub_sources(record['name'])
        pe = census.PE((folder / record['path']).read_bytes())
        names = {('#' + str(e['ordinal'])) if not e['names'] else e['names'][0] for e in pe.exports()}
        if (record.get('wine_reused') is not False or record.get('license') != 'LGPL-2.1-or-later'
                or record.get('source_sha256') != hashlib.sha256(source.encode()).hexdigest()
                or record.get('file_bytes') != len(pe.data) or record.get('memory') != memory_model(pe)
                or names != {e['symbol'] for e in exports}
                or inventory['exports'][record['name']] != exports):
            raise ValueError('absent-DLL stub license/source/exports mismatch: ' + record['name'])
        for item in pe.imports() + pe.imports(True):
            if item['dll'].lower() != 'ntdll.dll' or set(item['symbols']) != {'w32_shim_stub'}:
                raise ValueError('absent-DLL stub imports: ' + record['name'])


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
        if path.is_file() and path.suffix.lower() in ('.dll', '.drv', '.acm', '.ocx'):
            if path.name.lower() not in expected:
                raise ValueError('unlisted builtin PE: ' + path.name)
    reused = [record for record in store['dlls'] if record.get('wine_reused') is True]
    if not reused:
        return
    built = json.loads(UPPER_RECORD.read_text())['dlls'] if UPPER_RECORD.is_file() else {}
    for record in reused:
        name = record['name'].lower()
        entry = built.get(name)
        if entry is None:
            raise ValueError('unlisted reused Wine PE: ' + record['name'])
        image = Path(folder) / record['path']
        actual = sha(image) if image.is_file() else None
        if (record.get('sha256') != entry['sha256'] or actual != entry['sha256']
                or record.get('base') != entry['base'] or record.get('license') != entry['license']):
            raise ValueError('mismatched reused Wine PE: ' + record['name'])
        if entry['qualified'] is not True:
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
