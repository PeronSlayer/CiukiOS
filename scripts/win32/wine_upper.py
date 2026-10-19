#!/usr/bin/env python3
"""Offline deterministic Wine 10.0 P4 PE builds, import decisions, measurements.

Only the SHA-256-verified release archive is compiled, with the recorded
patches, clang and lld. The reused binaries stay unqualified (T1 rejects
them) until the decided core amendments exist and their loads pass.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import census, upper_dlls, wine_audit

MODULES = ('comctl32', 'comdlg32', 'shell32', 'shlwapi', 'ole32', 'combase',
           'oleaut32', 'rpcrt4', 'riched20', 'riched32', 'cabinet', 'advpack',
           'setupapi', 'imm32', 'msvcrt',
           # In-process pieces Wine 10.0 split out of OLE32 (structured
           # storage) and SHLWAPI (streams, registry, thread helpers); both
           # are eager imports of those modules.
           'coml2', 'shcore')
PATCHES = ('0001-in-process-rpc-only.patch', '0002-disable-rpcss-activation.patch',
           '0003-setupdi-unsupported.patch', '0004-single-msvcrt-crt.patch',
           '0005-builtin-region-base.patch')
CFLAGS = '-O2 -g0 -march=pentiumpro -mno-sse -mno-sse2 -mno-mmx -fno-ident'
LDFLAGS = '--no-default-config -Wl,-timestamp:0'
# Fixed CiukiOS builtin-region bases, packed in MODULES order with 64 KiB
# granularity above the authored builtins (which end below 0x71000000). The
# production loader maps a builtin only at its manifest base.
UPPER_BASE = 0x74000000
UPPER_LIMIT = 0x76000000


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def dump(path, data):
    Path(path).write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')


def verify_archive(archive, pin):
    if sha(archive) != pin['archive_sha256']:
        raise ValueError('Wine release archive SHA-256 mismatch')


def unpack(archive, destination):
    """No absolute paths, traversal, links, devices or writes outside source."""
    destination = Path(destination)
    with tarfile.open(archive, 'r:xz') as tar:
        for member in tar:
            parts = Path(member.name).parts
            if (not parts or parts[0] != 'wine-10.0' or
                    '..' in parts or Path(member.name).is_absolute() or
                    not (member.isfile() or member.isdir())):
                raise ValueError('unsafe Wine archive member: ' + member.name)
            target = destination.joinpath(*parts[1:])
            if not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError('Wine archive target escapes extraction directory')
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(member.mode & 0o777)


def run(args, cwd, log):
    env = dict(os.environ, LC_ALL='C', TZ='UTC', SOURCE_DATE_EPOCH='1737417600')
    # All tools and target compilation are sequential and low priority.
    subprocess.run(['nice', '-n', '19', *map(str, args)], cwd=cwd, env=env,
                   stdout=log, stderr=subprocess.STDOUT, check=True)


def build(output, archive=None):
    output = Path(output).resolve()
    archive = Path(archive or ROOT / 'build/downloads/wine-10.0.tar.xz').resolve()
    source = ROOT / 'build/external/wine-10.0'
    if output == source or source in output.parents or output in source.parents:
        raise ValueError('upper build must be separate from staged sources')
    pin = json.loads((ROOT / 'config/wine-pin.json').read_text())
    verify_archive(archive, pin)
    for name, info in pin['tools'].items():
        path = shutil.which(name)
        if not path or sha(path) != info['sha256']:
            raise ValueError('Missing or changed pinned build tool: ' + name)
    output.mkdir(parents=True, exist_ok=True)
    patched = output / 'source'
    # Never patch the shared staged source tree. A fresh extraction also prevents
    # unpinned source files or stale generated objects from entering a build.
    if patched.exists():
        shutil.rmtree(patched)
    patched.mkdir()
    unpack(archive, patched)
    work = output / 'objects'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    pe_dir = output / 'pe'
    pe_dir.mkdir(exist_ok=True)
    for name in MODULES:
        (pe_dir / (name + '.dll')).unlink(missing_ok=True)
    patch_hashes = {}
    with (output / 'build.log').open('w') as log:
        for name in PATCHES:
            path = ROOT / 'third_party/wine/patches' / name
            patch_hashes[name] = sha(path)
            run(['patch', '--batch', '--fuzz=0', '-p1', '-i', path], patched, log)
        run(['../source/configure', '--enable-archs=i386', '--disable-win16',
             '--disable-tests', '--disable-build-id', '--with-mingw=clang',
             '--without-x', '--without-wayland', '--without-freetype',
             '--without-fontconfig', '--without-vulkan', '--without-opengl',
             '--without-gnutls', 'CC=clang', 'CXX=clang++',
             'i386_CFLAGS=' + CFLAGS, 'i386_LDFLAGS=' + LDFLAGS], work, log)
        base = UPPER_BASE
        for name in MODULES:
            target = f'dlls/{name}/i386-windows/{name}.dll'
            run(['make', '-j1', 'i386_LDFLAGS=' + LDFLAGS + f' -Wl,-base:{base:#x}', target], work, log)
            # winegcc always appends -debug:dwarf outside macOS, which keeps a
            # COFF symbol table beyond SizeOfImage; strip it (no mapped bytes).
            run(['llvm-objcopy', '--strip-all', work / target, pe_dir / (name + '.dll')], work, log)
            pe = census.PE((pe_dir / (name + '.dll')).read_bytes())
            if pe.base != base:
                raise ValueError(f'{name}.dll linked at {pe.base:#x}, expected {base:#x}')
            base = (base + pe.size + 0xffff) & ~0xffff
            if base > UPPER_LIMIT:
                raise ValueError('upper DLLs exceed the reserved builtin region')
            print('Built ' + name + '.dll', flush=True)
    records = {}
    for name in MODULES:
        path = pe_dir / (name + '.dll')
        pe = census.PE(path.read_bytes())
        records[path.name] = {'sha256': sha(path), 'file_bytes': path.stat().st_size,
                              'base': pe.base, 'size_of_image': pe.size,
                              'memory': upper_dlls.memory_model(pe),
                              'wine_reused': True, 'license': 'LGPL-2.1-or-later',
                              'qualified': False}
    result = {'wine_commit': pin['commit'], 'archive_sha256': pin['archive_sha256'],
              'patches': patch_hashes, 'cflags': CFLAGS, 'ldflags': LDFLAGS,
              'base_region': [UPPER_BASE, UPPER_LIMIT], 'strip': 'llvm-objcopy --strip-all',
              'dlls': records, 'memory_model': upper_dlls.MEMORY_MODEL,
              'status': 'PE build evidence only; import/load gates required'}
    dump(output / 'build.json', result)
    return result


RECORD = ROOT / 'config/wine-upper.json'
# Built and measured, but not reused (decision recorded in the record).
NOT_REUSED = {'imm32.dll': 'The CiukiOS IMM32 W21 placeholder is smaller (3,072 file bytes, '
              '20,480 SizeOfImage versus 102,400) with the same no-IME behaviour; the Wine '
              'build also needs twelve WIN32U system-call exports that only Wine and '
              'Windows 10 provide.'}
CORE = ('ntdll.dll', 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll')
# Windows 98 home DLLs searched in the pinned Wine specs for a KERNELBASE name.
HOMES = ('kernel32', 'advapi32', 'user32', 'gdi32', 'version', 'shlwapi')
RULED = {
    'w21-services': ('advapi32.dll', re.compile(
        r'(Open|Create|Start|Delete|Control)Service\w*|OpenSCManager\w*|'
        r'(Query|Change)Service\w*|CloseServiceHandle')),
    'w21-nt-security': ('advapi32.dll', re.compile(
        r'(Get|Set)FileSecurity[AW]|CreateProcessAsUser[AW]|LookupPrivilegeValue[AW]|'
        r'ConvertSidToStringSid[AW]')),
    'w21-msi': ('advapi32.dll', re.compile(r'CommandLineFromMsiDescriptor')),
    'w21-dpi': ('user32.dll', re.compile(
        r'GetDpiForMonitorInternal|(Get|Set)ProcessDpiAwarenessInternal')),
    'core-security-structures': ('advapi32.dll', re.compile(
        r'Initialize(SecurityDescriptor|Acl)|AddAccess(Allowed|Denied)Ace(Ex)?|'
        r'SetSecurityDescriptor(Dacl|Owner|Group)|GetLengthSid')),
    'core-uniscribe': ('gdi32.dll', re.compile(r'Script\w+')),
    'w21-actctx': ('kernel32.dll', re.compile(
        r'(Create|Query)ActCtx[AW]?|(Activate|Deactivate|Release|AddRef)ActCtx|'
        r'GetCurrentActCtx|FindActCtxSection\w+')),
    'w21-wow64': ('kernel32.dll', re.compile(
        r'Wow64(Disable|Revert)Wow64FsRedirection|GetSystemWow64Directory[AW]')),
    'w21-thread-description': ('kernel32.dll', re.compile(r'(Set|Get)ThreadDescription')),
}
RULES = {
    'not-reused': 'The importing Wine build is measured but not reused; the entry '
                  'disappears with it (see not_reused).',
    'wine-private': 'Wine-private helper with no Windows 98 meaning; a Wine-side source '
                    'patch removes the call path. No Wine-private export enters the core.',
    'ntdll-runtime': 'Compiler/CRT helper that NT NTDLL exports (64-bit arithmetic, stack '
                     'probe, _vsnprintf); the CiukiOS NTDLL implements it (core amendment).',
    'ntdll-native': 'NT native API (Nt*/Rtl*/Ldr*) reached by the reused DLL; the CiukiOS '
                    'NTDLL implements it over its own services (core amendment).',
    'core-api': 'Documented Win32 API reached by the reused DLL; the owning CiukiOS core '
                'DLL implements it (amendment of the owning core directive).',
    'w21-services': 'Service Control Manager: Windows 9x has none and fails with '
                    'ERROR_CALL_NOT_IMPLEMENTED; W21 diagnostic stub with that failure.',
    'w21-nt-security': 'NT token/ACL services (file ACLs, privileges, user tokens, SID '
                       'strings) are absent from Windows 9x and FAT; Windows 9x fails with '
                       'ERROR_CALL_NOT_IMPLEMENTED; W21 stub with that failure.',
    'w21-msi': 'Windows Installer descriptors (advertised shortcuts) need MSI, which '
               'CiukiOS refuses (W32_REFUSE_MSI); W21 stub returning ERROR_CALL_NOT_IMPLEMENTED.',
    'w21-dpi': 'Per-monitor DPI internals (Windows 8.1+) reached only through SHCORE '
               'forwarders; W21 stub failing, callers fall back to 96 DPI.',
    'core-security-structures': 'Security descriptors and ACLs are plain data structures: '
                                'the CiukiOS ADVAPI32 builds them in NT layout and object '
                                'creation ignores them (no ACL enforcement on FAT); core amendment.',
    'core-uniscribe': 'Wine 10.0 moved Uniscribe (USP10) into GDI32 and RICHED20/COMCTL32 '
                      'shape text through it; the CiukiOS GDI32 implements a left-to-right '
                      'single-script subset (core amendment).',
    'w21-actctx': 'Activation contexts are Windows XP+; W21 stub failing with '
                  'ERROR_CALL_NOT_IMPLEMENTED, which the callers treat as no context.',
    'w21-wow64': 'WOW64 redirection exists only on 64-bit Windows; documented to fail '
                 'on 32-bit Windows; W21 stub with ERROR_CALL_NOT_IMPLEMENTED.',
    'w21-thread-description': 'Thread descriptions are Windows 10 diagnostics; W21 stub '
                              'returning E_NOTIMPL.',
    'kernelbase-retarget': 'Wine 10.0 links KERNELBASE, which Windows 98 lacks; a Wine-side '
                           'Makefile/spec patch takes the function from its Windows 98 home '
                           'DLL (pinned Wine spec), where the CiukiOS core already exports it.',
    'kernelbase-shlwapi-compile-in': 'SHLWAPI function that Wine 10.0 moved to KERNELBASE; '
                                     'Windows 98 SHLWAPI implemented it. Wine-side patch: '
                                     'compile the upstream kernelbase unit into the reused SHLWAPI.',
    'kernelbase-to-shlwapi': 'SHLWAPI function taken from KERNELBASE by another reused DLL; '
                             'Wine-side patch retargets it to the reused SHLWAPI (after its compile-in).',
    'kernelbase-internal': 'KERNELBASE-only API without a Windows 98 home; Wine-side patch '
                           'compiles the upstream kernelbase unit into the importing DLL.',
    'ciukios-stub-dll': 'Provided by an existing CiukiOS census/W21 DLL that lacks the export; '
                        'add a W21 stub with the documented failure (no printer, no IME, no '
                        'network provider).',
    'ciukios-version': 'CiukiOS VERSION implements the ANSI functions; the Unicode functions '
                       'are added (core amendment).',
    'absent-delayed-dll': 'Delay-imported DLL absent from CiukiOS; a generated CiukiOS W21 stub '
                          'DLL returns the documented failure per function (stub signatures), '
                          'so delay binding succeeds and the caller sees failure (no theme, '
                          'codec, trust provider or network).',
    'absent-forwarder-dll': 'Export forwarder into a DLL absent from CiukiOS; forwarders bind '
                            'lazily, so the DLL loads and a binder of this export gets the W21 '
                            'missing-export diagnostic.',
}
ACTIONS = {'not-reused': 'not-reused', 'wine-private': 'wine-patch-remove',
           'ntdll-runtime': 'core-implement', 'ntdll-native': 'core-implement',
           'core-api': 'core-implement', 'ciukios-version': 'core-implement',
           'w21-services': 'core-w21-stub', 'w21-nt-security': 'core-w21-stub',
           'w21-msi': 'core-w21-stub', 'w21-dpi': 'core-w21-stub',
           'core-security-structures': 'core-implement', 'core-uniscribe': 'core-implement',
           'w21-actctx': 'core-w21-stub', 'w21-wow64': 'core-w21-stub',
           'w21-thread-description': 'core-w21-stub', 'ciukios-stub-dll': 'core-w21-stub',
           'absent-delayed-dll': 'w21-stub-dll',
           'kernelbase-retarget': 'wine-retarget', 'kernelbase-to-shlwapi': 'wine-retarget',
           'kernelbase-shlwapi-compile-in': 'wine-compile-in',
           'kernelbase-internal': 'wine-compile-in',
           'absent-forwarder-dll': 'w21-unresolved-forwarder'}


def providers_dir(output, core):
    """CiukiOS core PEs plus the reused Wine PEs (not-reused builds excluded)."""
    output, core = Path(output).resolve(), Path(core).resolve()
    providers = output / 'providers'
    if providers.exists():
        shutil.rmtree(providers)
    providers.mkdir()
    for directory in (core, output / 'pe'):
        for path in sorted(directory.iterdir()):
            name = path.name.lower()
            if name.endswith(('.dll', '.acm', '.drv')) and path.is_file():
                if directory != core and name in NOT_REUSED:
                    continue
                dest = providers / name
                dest.unlink(missing_ok=True)
                dest.symlink_to(path)
    return providers


def classify(importer, provider, name, core_exports, specs, ciukios):
    """One decision for one missing (provider, name) of one reused importer."""
    provider = provider.lower()
    stem = provider.rsplit('.', 1)[0]
    if importer in NOT_REUSED:
        return 'not-reused', importer, None
    if name.startswith(('wine_', '__wine_')):
        return 'wine-private', importer, None
    if provider == 'kernelbase.dll':
        homes = [h for h in HOMES if name in specs.get(h, {})
                 and '-private' not in specs[h][name]['flags']]
        if not homes:
            return 'kernelbase-internal', importer, None
        home = homes[0] + '.dll'
        if home == 'shlwapi.dll':
            if importer == 'shlwapi.dll':
                return 'kernelbase-shlwapi-compile-in', 'shlwapi.dll', None
            return 'kernelbase-to-shlwapi', 'shlwapi.dll', 'shlwapi.dll'
        if name in core_exports.get(home, ()):
            return 'kernelbase-retarget', home, home
        rule, owner, _ = classify(importer, home, name, core_exports, specs, ciukios)
        return rule, owner, home
    for rule, (owner, pattern) in RULED.items():
        if provider == owner and pattern.fullmatch(name):
            return rule, owner, None
    if provider == 'ntdll.dll':
        if name.startswith('_') or name[:1].islower():
            return 'ntdll-runtime', provider, None
        if name.startswith(('Nt', 'Zw', 'Rtl', 'Ldr')):
            return 'ntdll-native', provider, None
    if provider in CORE:
        return 'core-api', provider, None
    if provider == 'version.dll':
        return 'ciukios-version', provider, None
    if provider in ciukios:
        return 'ciukios-stub-dll', provider, None
    raise ValueError(f'no decision rule: {importer} -> {provider}!{name}')


def decide(rows, providers, specs):
    images = upper_dlls.read_images(providers)
    core_exports = {name: {n for e in meta['exports'] for n in e['names']}
                    for name, (_, meta) in images.items()}
    wine = {m + '.dll' for m in MODULES}
    ciukios = {name for name in images if name not in wine or name in NOT_REUSED}
    decisions = []
    for importer, row in sorted(rows.items()):
        entries = []
        for item in row['imports']:
            for symbol in item['missing']:
                chain = item['resolutions'][symbol]['chain']
                final = chain[-1]
                provider, _, name = final.partition('!')
                kind = 'delay-import' if item['delayed'] else 'import'
                absent = item['resolutions'][symbol]['error'] == 'missing-dll'
                entries.append((kind, item['dll'], symbol, provider, name, absent))
        for forward in row['forwarders']:
            if forward['error']:
                provider, _, name = forward['chain'][-1].partition('!')
                entries.append(('forwarder', importer, '#' + str(forward['ordinal']),
                                provider, name, forward['error'] == 'missing-dll'))
        for kind, linked, symbol, provider, name, absent in entries:
            if importer in NOT_REUSED:
                rule, owner, retarget = 'not-reused', importer, None
            elif absent and provider != 'kernelbase.dll' and provider not in ciukios:
                rule = 'absent-forwarder-dll' if kind == 'forwarder' else 'absent-delayed-dll'
                if kind == 'import':
                    raise ValueError(f'eager import of an absent DLL: {importer} -> {provider}')
                owner, retarget = provider, None
            else:
                rule, owner, retarget = classify(importer, provider, name, core_exports, specs, ciukios)
                if kind == 'forwarder' and provider == 'kernelbase.dll' and owner == 'shlwapi.dll':
                    rule, retarget = 'kernelbase-to-shlwapi', 'shlwapi.dll'
            decisions.append({'importer': importer, 'kind': kind, 'linked': linked,
                              'symbol': symbol, 'provider': provider, 'name': name,
                              'owner': owner, 'retarget': retarget, 'rule': rule,
                              'action': ACTIONS[rule]})
    return decisions


def summarize(rows):
    """Compact per-DLL import table: required/present/missing per provider."""
    result = {}
    for dll, row in sorted(rows.items()):
        imports = {}
        for item in row['imports']:
            key = item['dll'] + (' (delayed)' if item['delayed'] else '')
            imports[key] = {'required': len(item['required']), 'present': len(item['present']),
                            'missing': item['missing']}
        result[dll] = {'imports': imports,
                       'forwarders': len(row['forwarders']),
                       'forwarders_unresolved': sorted(f['target'] for f in row['forwarders'] if f['error']),
                       'eager_missing': sum(len(item['missing']) for item in row['imports']
                                            if not item['delayed'])}
    return result


def audit(output, core):
    """Actual eager/delayed imports and recursive forwarders, one decision each."""
    output, core = Path(output).resolve(), Path(core).resolve()
    providers = providers_dir(output, core)
    names = [n + '.dll' for n in MODULES]
    rows = upper_dlls.audit_images(providers, [n for n in names if n not in NOT_REUSED])
    for name in NOT_REUSED:
        # Audited against the same providers with the Wine build in place.
        alone = output / ('providers-' + name.removesuffix('.dll'))
        if alone.exists():
            shutil.rmtree(alone)
        shutil.copytree(providers, alone, symlinks=True)
        (alone / name).unlink(missing_ok=True)
        (alone / name).symlink_to(output / 'pe' / name)
        rows[name] = upper_dlls.audit_images(alone, [name])[name]
        shutil.rmtree(alone)
    pin = json.loads(wine_audit.PIN.read_text())
    specs = wine_audit.validate(json.loads(wine_audit.AUDIT.read_text()), pin)
    decisions = decide(rows, providers, specs)
    core_hashes = {p.name.lower(): sha(p) for p in sorted(core.iterdir())
                   if p.suffix.lower() in ('.dll', '.drv', '.acm') and p.is_file()}
    result = {'imports': summarize(rows), 'decisions': decisions, 'core_pe_sha256': core_hashes}
    dump(output / 'import-audit.json', rows)
    dump(output / 'decisions.json', result)
    return result


def libc_surface(audit_rows):
    """MSVCRT functions the reused DLLs import (the measured CRT surface)."""
    names = set()
    for dll, row in audit_rows.items():
        if dll in NOT_REUSED:
            continue
        for item in row['imports']:
            if item['dll'] == 'msvcrt.dll':
                names.update(item['required'])
    return sorted(names)


# MSVCRT names whose C-standard/POSIX counterpart exists in newlib.
NEWLIB_ALIASES = {'_strdup': 'strdup', '_wcsdup': 'wcsdup', '_strnicmp': 'strncasecmp',
                  '_stricmp': 'strcasecmp', '_wcsicmp': 'wcscasecmp', '_wcsnicmp': 'wcsncasecmp',
                  '_close': 'close', '_read': 'read', '_write': 'write', '_open': 'open',
                  '_lseek': 'lseek', '_wtoi': None, '_snprintf': 'snprintf',
                  '_vsnprintf': 'vsnprintf', '_snwprintf': 'swprintf', '_vsnwprintf': 'vswprintf',
                  '_isnan': 'isnan', '_finite': 'finite', '_hypot': 'hypot', '_strlwr': None,
                  '_errno': '__errno', '_fstat': 'fstat', '_msize': 'malloc_usable_size'}


def msvcrt_measure(output, sdk, surface):
    """Wine MSVCRT PE versus the CiukiOS newlib port for the same surface.

    The newlib side is measured, not modelled: a static ELF referencing every
    surface function with a newlib counterpart is linked with the installed
    CiukiOS SDK (--gc-sections), minus an empty baseline program.
    """
    output, sdk = Path(output), Path(sdk)
    work = output / 'msvcrt-measure'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    cc = sdk / 'bin/ciuki-cc'
    symbols = {}
    for name in surface:
        alias = NEWLIB_ALIASES.get(name, name if not name.startswith('_') else None)
        symbols[name] = alias
    probe = work / 'nm.txt'
    with probe.open('w') as stream:
        subprocess.run(['llvm-nm', '--defined-only', '-g', sdk / 'sysroot/lib/libc.a',
                        sdk / 'sysroot/lib/libm.a', sdk / 'sysroot/lib/libciuki.a'],
                       stdout=stream, stderr=subprocess.DEVNULL,
                       check=True)
    defined = set(re.findall(r'^[0-9a-f]+ [TDRBW] (\S+)$', probe.read_text(), re.M))
    present = {name: alias for name, alias in symbols.items() if alias and alias in defined}
    missing = sorted(name for name in surface if name not in present)
    body = ''.join(f'extern char {alias}[];\n' for alias in sorted(set(present.values())))
    body += 'void *const surface[] = {' + ', '.join(f'(void *){a}' for a in sorted(set(present.values()))) + '};\n'
    # A volatile read keeps the table (and through it every function) alive
    # under --gc-sections; constant folding would otherwise drop it.
    body += 'int main(void) { void *const *volatile p = surface; return p[0] == 0; }\n'
    (work / 'surface.c').write_text(body)
    (work / 'empty.c').write_text('int main(void) { return 0; }\n')
    sizes = {}
    for stem in ('empty', 'surface'):
        elf = work / (stem + '.elf')
        subprocess.run([sys.executable, cc, '-O2', '-fno-builtin', '-Wl,--gc-sections', '-ffunction-sections',
                        work / (stem + '.c'), '-o', elf], check=True, capture_output=True)
        text = subprocess.check_output(['llvm-size', '-A', elf], text=True)
        total = {'text': 0, 'data': 0, 'bss': 0}
        for line in text.splitlines():
            parts = line.split()
            if len(parts) < 3 or not parts[1].isdigit():
                continue
            section, size = parts[0], int(parts[1])
            if section in ('.text', '.init', '.fini') or section.startswith(('.text.', '.rodata')):
                total['text'] += size
            elif section.startswith(('.data', '.init_array', '.fini_array', '.tdata')):
                total['data'] += size
            elif section.startswith(('.bss', '.tbss')):
                total['bss'] += size
        sizes[stem] = total
    delta = {k: sizes['surface'][k] - sizes['empty'][k] for k in sizes['surface']}
    pages = lambda n: (n + 4095) // 4096
    wine_pe = output / 'pe' / 'msvcrt.dll'
    pe = census.PE(wine_pe.read_bytes())
    wine_exports = {n for e in pe.exports() for n in e['names']}
    return {'surface': surface, 'surface_count': len(surface),
            'wine': {'file_bytes': wine_pe.stat().st_size, 'size_of_image': pe.size,
                     'memory': upper_dlls.memory_model(pe),
                     'surface_exported': sum(n in wine_exports for n in surface),
                     'exports': len(wine_exports)},
            'newlib': {'sdk_libc_sha256': sha(sdk / 'sysroot/lib/libc.a'),
                       'covered': len(present), 'missing': missing,
                       'linked_bytes_over_empty': delta,
                       'startup_pages_estimate': pages(delta['text']) + pages(delta['data'] + delta['bss'])},
            'method': 'Wine side: actual PE and section-store model. Newlib side: static '
                      'CiukiOS SDK ELF with every covered surface function referenced, '
                      '--gc-sections, minus an empty program; MSVCRT ABI glue (FILE/_iob '
                      'layout, _errno, locale, SEH/C++ EH, startup) not included.'}


def fit(build_record, not_reused):
    """128 MiB fit figures: sums against the contract's 14 MiB installer budget."""
    dlls = {n: r for n, r in build_record.items() if n not in not_reused}
    total = lambda key: sum(r[key] for r in dlls.values())
    mem = lambda key: sum(r['memory'][key] for r in dlls.values())
    budget = 14 * 1024 * 1024
    shell = dlls.get('shell32.dll')
    return {'reused_dlls': sorted(dlls), 'file_bytes': total('file_bytes'),
            'size_of_image': total('size_of_image'),
            'shared_seed_pages': mem('shared_seed_pages'),
            'private_cow_commitment_pages': mem('private_cow_commitment_pages'),
            'startup_committed_pages_model': mem('startup_committed_pages_model'),
            'installer_budget_bytes': budget,
            'size_of_image_within_budget': total('size_of_image') <= budget,
            'seed_bytes_within_budget': mem('shared_seed_pages') * 4096 <= budget,
            'shell32_size_of_image': shell['size_of_image'] if shell else None,
            'model': upper_dlls.MEMORY_MODEL}


def resource_entries(data):
    """Resource directory entries (all levels), as the PE parser walks them."""
    pe = census.PE(data)
    rva, size = pe.dirs[2]
    if not rva or not size:
        return 0
    def offset(target):
        for s in pe.sections:
            if s['rva'] <= target < s['rva'] + max(s['span'], s['raw_size']):
                return s['offset'] + target - s['rva']
        raise ValueError('resource RVA outside sections')
    base, count, todo = offset(rva), 0, [0]
    while todo:
        at = base + todo.pop()
        n = int.from_bytes(data[at + 12:at + 14], 'little') + int.from_bytes(data[at + 14:at + 16], 'little')
        for i in range(n):
            count += 1
            target = int.from_bytes(data[at + 20 + 8 * i:at + 24 + 8 * i], 'little')
            if target & 0x80000000:
                todo.append(target & 0x7fffffff)
    return count


def record(output, sdk, runs):
    """Write config/wine-upper.json from a build, a second identical run and the audit."""
    output = Path(output)
    build_record = json.loads((output / 'build.json').read_text())
    for other in runs:
        if json.loads(Path(other).read_text())['dlls'] != build_record['dlls']:
            raise ValueError('upper builds differ between runs: ' + str(other))
    decisions = json.loads((output / 'decisions.json').read_text())
    rows = json.loads((output / 'import-audit.json').read_text())
    surface = libc_surface(rows)
    result = {'schema': 1, 'wine_commit': build_record['wine_commit'],
              'archive_sha256': build_record['archive_sha256'],
              'recipe': {k: build_record[k] for k in ('cflags', 'ldflags', 'strip', 'base_region',
                                                      'patches')},
              'modules': list(MODULES), 'identical_runs': 1 + len(runs),
              'dlls': {n: {**{k: r[k] for k in ('sha256', 'file_bytes', 'base', 'size_of_image',
                                                 'memory', 'license', 'qualified')},
                           'resource_entries': resource_entries((output / 'pe' / n).read_bytes())}
                       for n, r in sorted(build_record['dlls'].items())},
              'not_reused': NOT_REUSED, 'rules': RULES,
              'imports': decisions['imports'], 'decisions': decisions['decisions'],
              'core_pe_sha256': decisions['core_pe_sha256'],
              'msvcrt': msvcrt_measure(output, sdk, surface),
              'fit': fit(build_record['dlls'], NOT_REUSED),
              'status': 'built twice identically; unqualified: decided core amendments and '
                        'Wine-side retarget/compile-in patches pending; T1 rejects the PEs'}
    validate(result)
    dump(RECORD, result)
    return result


def validate(data, patches=ROOT / 'third_party/wine/patches'):
    """Self-consistency of the committed record (offline; no build needed)."""
    if list(data['recipe']['patches']) != list(PATCHES):
        raise ValueError('patch order differs from the recipe')
    for name, digest in data['recipe']['patches'].items():
        if sha(Path(patches) / name) != digest:
            raise ValueError('patch hash mismatch: ' + name)
    if set(data['dlls']) != {m + '.dll' for m in MODULES}:
        raise ValueError('built module set differs from the recipe')
    low, high = data['recipe']['base_region']
    spans = sorted((r['base'], r['base'] + r['size_of_image'], n) for n, r in data['dlls'].items())
    for (start, end, name), following in zip(spans, spans[1:] + [(high, high, '')]):
        if start < low or start & 0xffff or end > following[0]:
            raise ValueError('reused DLL base outside region or overlapping: ' + name)
        if data['dlls'][name]['qualified'] is not False:
            raise ValueError('reused DLL marked qualified: ' + name)
        if data['dlls'][name]['license'] != 'LGPL-2.1-or-later':
            raise ValueError('reused DLL licence: ' + name)
    seen = set()
    for entry in data['decisions']:
        key = (entry['importer'], entry['kind'], entry['linked'], entry['symbol'])
        if key in seen:
            raise ValueError('duplicate decision: ' + repr(key))
        seen.add(key)
        if entry['rule'] not in RULES or ACTIONS[entry['rule']] != entry['action']:
            raise ValueError('unknown decision rule/action: ' + repr(key))
    expected = set()
    for dll, row in data['imports'].items():
        for provider, item in row['imports'].items():
            kind = 'delay-import' if provider.endswith(' (delayed)') else 'import'
            for symbol in item['missing']:
                expected.add((dll, kind, provider.removesuffix(' (delayed)'), symbol))
        if len(row['forwarders_unresolved']) != sum(
                1 for e in data['decisions'] if e['importer'] == dll and e['kind'] == 'forwarder'):
            raise ValueError('forwarder decisions differ from the audit: ' + dll)
    if expected != {k for k in seen if k[1] != 'forwarder'}:
        raise ValueError('missing exports without exactly one decision')
    return True


DOCUMENT = ROOT / 'docs/design/win32-wine-dependency-audit.md'
BEGIN = '<!-- BEGIN GENERATED WINE UPPER -->'
END = '<!-- END GENERATED WINE UPPER -->'


def render(data):
    """Deterministic Markdown tables of the committed record."""
    from collections import Counter, defaultdict
    out = [BEGIN, '', '### Reused upper DLL builds (generated from `config/wine-upper.json`)', '',
           f"Wine `{data['wine_commit']}`, archive SHA-256 `{data['archive_sha256']}`, "
           f"{data['identical_runs']} consecutive runs with identical hashes. "
           f"CFLAGS `{data['recipe']['cflags']}`, LDFLAGS `{data['recipe']['ldflags']}`, "
           f"`{data['recipe']['strip']}`.", '',
           '| DLL | Base | File bytes | SizeOfImage | Seed pages | COW pages | Resource entries | SHA-256 (prefix) | Use |',
           '| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |']
    for name in [m + '.dll' for m in data['modules']]:
        r = data['dlls'][name]
        use = 'measured, not reused' if name in data['not_reused'] else 'reused (unqualified)'
        out.append(f"| {name} | `{r['base']:#010x}` | {r['file_bytes']:,} | {r['size_of_image']:,} | "
                   f"{r['memory']['shared_seed_pages']} | {r['memory']['private_cow_commitment_pages']} | "
                   f"{r['resource_entries']:,} | `{r['sha256'][:16]}` | {use} |")
    out += ['', '| Patch (applied in order) | SHA-256 (prefix) |', '| --- | --- |']
    out += [f'| `{name}` | `{digest[:16]}` |' for name, digest in data['recipe']['patches'].items()]
    out += ['', '### Import decisions (generated)', '',
            'Every missing eager import, delayed import and unresolved export forwarder of every '
            'build has exactly one decision (`decisions` in the record; the final forwarder hop '
            'is classified).', '',
            '| DLL | Eager missing | Delayed missing | Unresolved forwarders |',
            '| --- | ---: | ---: | ---: |']
    for name, row in data['imports'].items():
        delayed = sum(len(i['missing']) for k, i in row['imports'].items() if k.endswith(' (delayed)'))
        out.append(f"| {name} | {row['eager_missing']} | {delayed} | {len(row['forwarders_unresolved'])} |")
    actions = Counter(e['action'] for e in data['decisions'])
    out += ['', '| Action | Entries |', '| --- | ---: |']
    out += [f'| `{a}` | {n} |' for a, n in sorted(actions.items())]
    groups = defaultdict(set)
    for e in data['decisions']:
        groups[(e['rule'], e['owner'])].add(e['name'])
    out += ['', '| Rule | Action | Decision |', '| --- | --- | --- |']
    rules = sorted({e['rule'] for e in data['decisions']})
    out += [f"| `{r}` | `{ACTIONS[r]}` | {data['rules'][r]} |" for r in rules]
    out += ['', 'Unique names per rule and owning DLL:', '']
    for (rule, owner), names in sorted(groups.items()):
        out.append(f"- `{rule}` → {owner} ({len(names)}): " + ', '.join(sorted(names)))
    m = data['msvcrt']
    out += ['', '### MSVCRT measurement (generated)', '',
            f"Surface: the {m['surface_count']} MSVCRT functions the reused DLLs import.", '',
            '| Candidate | Bytes | Pages (model) | Surface covered | Notes |',
            '| --- | ---: | ---: | ---: | --- |',
            f"| Wine 10.0 `msvcrt.dll` PE | {m['wine']['file_bytes']:,} file, "
            f"{m['wine']['size_of_image']:,} SizeOfImage | "
            f"{m['wine']['memory']['startup_committed_pages_model']} "
            f"({m['wine']['memory']['shared_seed_pages']} seed + "
            f"{m['wine']['memory']['private_cow_commitment_pages']} COW) | "
            f"{m['wine']['surface_exported']}/{m['surface_count']} | "
            f"{m['wine']['exports']} exported names, MSVCRT ABI |",
            f"| CiukiOS newlib port (SDK `libc.a` `{m['newlib']['sdk_libc_sha256'][:16]}`) | "
            f"{m['newlib']['linked_bytes_over_empty']['text']:,} text + "
            f"{m['newlib']['linked_bytes_over_empty']['data'] + m['newlib']['linked_bytes_over_empty']['bss']:,} data | "
            f"{m['newlib']['startup_pages_estimate']} | {m['newlib']['covered']}/{m['surface_count']} | "
            f"missing: {', '.join(m['newlib']['missing'])}; no MSVCRT ABI |", '',
            m['method']]
    f = data['fit']
    out += ['', '### 128 MiB fit figures (generated)', '',
            '| Figure | Value |', '| --- | ---: |',
            f"| Reused DLLs | {len(f['reused_dlls'])} |",
            f"| File bytes | {f['file_bytes']:,} |",
            f"| SizeOfImage | {f['size_of_image']:,} |",
            f"| Shared seed pages | {f['shared_seed_pages']} ({f['shared_seed_pages'] * 4096:,} bytes) |",
            f"| Private COW commitment pages | {f['private_cow_commitment_pages']} |",
            f"| Startup committed pages (model) | {f['startup_committed_pages_model']} |",
            f"| Contract installer budget (P2+P4) | {f['installer_budget_bytes']:,} |",
            f"| SizeOfImage within budget | {'yes' if f['size_of_image_within_budget'] else 'no'} |",
            f"| SHELL32 SizeOfImage | {f['shell32_size_of_image']:,} |", '',
            f['model'], '', END]
    return '\n'.join(out)


def write_document(data):
    document = DOCUMENT.read_text()
    generated = render(data)
    if BEGIN in document:
        start, end = document.index(BEGIN), document.index(END) + len(END)
        document = document[:start] + generated + document[end:]
    else:
        document = document.rstrip() + '\n\n' + generated + '\n'
    DOCUMENT.write_text(document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('build')
    p.add_argument('output', type=Path)
    p.add_argument('--archive', type=Path)
    p = sub.add_parser('audit')
    p.add_argument('output', type=Path)
    p.add_argument('core', type=Path)
    p = sub.add_parser('record', help='write config/wine-upper.json')
    p.add_argument('output', type=Path)
    p.add_argument('sdk', type=Path)
    p.add_argument('runs', type=Path, nargs='+', help='build.json of the other identical runs')
    sub.add_parser('check')
    sub.add_parser('render')
    args = parser.parse_args()
    if args.command == 'build':
        build(args.output, args.archive)
    elif args.command == 'audit':
        audit(args.output, args.core)
    elif args.command == 'record':
        record(args.output, args.sdk, args.runs)
    elif args.command == 'render':
        data = json.loads(RECORD.read_text())
        validate(data)
        write_document(data)
    else:
        data = json.loads(RECORD.read_text())
        validate(data)
        if render(data) not in DOCUMENT.read_text():
            raise ValueError('generated upper DLL tables differ from the record')
        print('PASS reused Wine upper DLL record')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f'wine upper: {exc}')
