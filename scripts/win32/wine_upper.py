#!/usr/bin/env python3
"""Offline deterministic Wine 10.0 P4 PE builds, import decisions, measurements.

Only the SHA-256-verified release archive is compiled, with the recorded
patches, clang and lld. `cached-build` is the step of `make build-full`
(`make upper`): the PEs are rebuilt only when the archive, the patch set or
the recipe changes, re-audited only when the core's export surface changes,
a build that does not reproduce config/wine-upper.json or an audit that
differs from its decisions is refused, and the record's core-DLL hashes are
refreshed. A reused PE is staged in the image only when its recorded H0/H1
load evidence qualifies it (QUALIFIED, qualified_by_evidence).
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
import time

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
           'coml2', 'shcore',
           # f6-26: DirectPlay 2/3/4, legacy exports and LAN provider.
           'dplayx', 'dplay', 'dpwsockx', 'oledlg', 'wininet')
PATCHES = ('0001-in-process-rpc-only.patch', '0002-disable-rpcss-activation.patch',
           '0003-setupdi-unsupported.patch', '0004-single-msvcrt-crt.patch',
           '0005-builtin-region-base.patch',
           # f5-14B amendment 2: deterministic resource filter, not a text
           # patch (the .ico inputs are binary); spec and result hashes in the file.
           '0006-shell32-icon-trim.json',
           # f5-20C: KERNELBASE retarget, kernelbase compile-in, Wine-private calls.
           '0007-kernelbase-retarget.patch', '0008-kernelbase-compile-in.patch',
           '0009-shell32-wine-private-calls.patch',
           # f5-20C, found by the H1 DllMain run: MSVCRT's C locale code page.
           '0010-msvcrt-sbcs-code-page.patch',
           '0011-directplay-lan.patch', '0012-directplay-lobby.patch',
           '0013-directplay-enumeration.patch', '0014-directplay-ipx.patch', '0015-directplay-lobby-lifetime.patch', '0016-wininet-no-tls.patch')
CFLAGS = '-O2 -g0 -march=pentiumpro -mno-sse -mno-sse2 -mno-mmx -fno-ident'
LDFLAGS = '--no-default-config -Wl,-timestamp:0'
# Fixed CiukiOS builtin-region bases, packed in MODULES order with 64 KiB
# granularity above the authored builtins (which end below 0x71000000). The
# production loader maps a builtin only at its manifest base.
UPPER_BASE = 0x74000000
UPPER_LIMIT = 0x76000000
# The cached step of make build-full (make upper): PEs, audit and cache key.
CACHE = ROOT / 'build/win32/upper'
CORE_DIR = ROOT / 'build/apps/win32'
ARCHIVE = ROOT / 'build/downloads/wine-10.0.tar.xz'


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


PNG = b'\x89PNG\r\n\x1a\n'


def trim_icon(data, keep):
    """One .ico reduced to the kept images (order kept, offsets recomputed)."""
    import struct
    reserved, kind, count = struct.unpack_from('<HHH', data, 0)
    if reserved or kind != 1 or not count or 6 + 16 * count > len(data):
        raise ValueError('not an icon directory')
    kept = []
    for i in range(count):
        entry = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        size, offset = entry[6], entry[7]
        image = data[offset:offset + size]
        if len(image) != size or size < 40:
            raise ValueError('icon image outside the file')
        if image[:8] == PNG:
            continue
        header, width, height, _, bits = struct.unpack_from('<IiiHH', image, 0)
        if header < 40:
            raise ValueError('icon image without BITMAPINFOHEADER')
        if bits <= keep['max_bit_count'] and width in keep['sizes'] and height == 2 * width:
            kept.append((entry, image))
    if not kept:
        raise ValueError('icon without a kept image')
    out, offset = struct.pack('<HHH', 0, 1, len(kept)), 6 + 16 * len(kept)
    for entry, image in kept:
        out += struct.pack('<BBBBHHII', *entry[:6], len(image), offset)
        offset += len(image)
    return out + b''.join(image for _, image in kept), [count, len(kept)]


def icon_trim(spec, source, verify=True):
    """Patch 0006: keep only Windows 98 icon formats in the listed directory.

    With verify, every input and output must match the recorded SHA-256 and
    image counts, so the filter is as reproducible as a text patch."""
    directory = Path(source) / spec['directory']
    names = sorted(p.name for p in directory.glob('*.ico'))
    if verify and names != sorted(spec['files']):
        raise ValueError('icon set differs from the 0006 record')
    files, totals = {}, {'images_before': 0, 'images_after': 0, 'bytes_before': 0, 'bytes_after': 0}
    for name in names:
        path = directory / name
        data = path.read_bytes()
        out, counts = trim_icon(data, spec['keep'])
        files[name] = {'before': hashlib.sha256(data).hexdigest(),
                       'after': hashlib.sha256(out).hexdigest(), 'images': counts}
        if verify and files[name] != spec['files'][name]:
            raise ValueError('0006 icon trim result differs: ' + name)
        path.write_bytes(out)
        totals['images_before'] += counts[0]
        totals['images_after'] += counts[1]
        totals['bytes_before'] += len(data)
        totals['bytes_after'] += len(out)
    return files, totals


def apply_filter(path, source):
    spec = json.loads(Path(path).read_text())
    if spec.get('filter') != 'icon-trim':
        raise ValueError('unknown recipe filter: ' + Path(path).name)
    return icon_trim(spec, source)[1]


def run(args, cwd, log):
    env = dict(os.environ, LC_ALL='C', TZ='UTC', SOURCE_DATE_EPOCH='1737417600')
    # All tools and target compilation are sequential and low priority.
    subprocess.run(['nice', '-n', '19', *map(str, args)], cwd=cwd, env=env,
                   stdout=log, stderr=subprocess.STDOUT, check=True)


def build(output, archive=None):
    output = Path(output).resolve()
    archive = Path(archive or ARCHIVE).resolve()
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
    patch_hashes, filters = {}, {}
    with (output / 'build.log').open('w') as log:
        for name in PATCHES:
            path = ROOT / 'third_party/wine/patches' / name
            patch_hashes[name] = sha(path)
            if name.endswith('.patch'):
                run(['patch', '--batch', '--fuzz=0', '-p1', '-i', path], patched, log)
            else:
                filters[name] = apply_filter(path, patched)
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
              'patches': patch_hashes, 'filters': filters, 'cflags': CFLAGS, 'ldflags': LDFLAGS,
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
# Build candidates stay out of the image until the lead can execute their
# DllMains and qualify the runtime paths. H0 binding is insufficient.
PENDING_QUALIFICATION = {
    name + '.dll': 'f6-26: H0 binds; H1 execution is blocked by sandbox SIGSYS; '
                   'DirectPlay end-to-end and Windows 98 interoperability gates remain'
    for name in ('dplayx', 'dplay', 'dpwsockx')}
PENDING_QUALIFICATION.update({name + '.dll': 'fx-hlutils: import closure and H0/H1 qualification required'
                              for name in ('oledlg', 'wininet')})
# Reused PEs that join the image's builtin store once their recorded host
# evidence passes (record 'load': H0 binding and H1 DllMain attach/detach on
# the PE with this exact hash; qualified_by_evidence). f5-20D (the cached
# upper build is a step of make build-full): MSVCRT, the f5-14B decision, and
# every other reused PE as its load/DllMain evidence qualifies it; the guest
# case w32-upper-dlls-qemu-t23 loads the staged set through the production
# loader and is the gate for its use. Removing a name keeps its PE out of the
# image. IMM32 is measured only (NOT_REUSED).
QUALIFIED = {m + '.dll': 'f5-20D: staged on the recorded H0 binding and H1 DllMain evidence'
             for m in MODULES if m + '.dll' not in NOT_REUSED
             and m + '.dll' not in PENDING_QUALIFICATION}
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
    'core-api-f5-20b': 'Win32 export outside the f5-14B table: imported by a reused DLL but then '
                       'exported by the core only as a census or Unicode W21 stub (so the audit did '
                       'not report it missing), or the ANSI/Unicode partner of a decided name; '
                       'implemented by f5-20B (MSLU model for the W forms; amendment 2 item 1).',
    'core-census-w21': 'Import bound to an existing CiukiOS census or typed W21 stub (documented '
                       'failure plus one W21 diagnostic); the reused DLL handles the failure. '
                       'Recorded by the f5-20C re-audit (no Wine or core change).',
    'core-api-f5-20c': 'Import bound to a nonreturning KERNEL32 census stub when the f5-20C re-audit '
                       'ran; implemented in f5-20C (lead decision): IsBadReadPtr/IsBadWritePtr/'
                       'IsBadCodePtr probe the range through the virtual-memory query and never '
                       'fault, DebugBreak raises EXCEPTION_BREAKPOINT (int 3) as Windows does.',
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
           'absent-forwarder-dll': 'w21-unresolved-forwarder',
           'core-api-f5-20b': 'core-implement', 'core-census-w21': 'core-w21-stub',
           'core-api-f5-20c': 'core-implement'}
# Census stubs the reused DLLs bound, implemented in f5-20C (lead decision).
F5_20C_IMPLEMENTED = {'kernel32.dll': ('DebugBreak', 'IsBadCodePtr', 'IsBadReadPtr', 'IsBadWritePtr')}
# f5-20B exports outside the f5-14B decision table (amendment 2 item 1): the
# exports f5-20B implemented which neither the f5-14B table nor the pre-f5-20B
# core (main 4ed78860, compiled exports) had. user32 87, gdi32 4 (gdi32 also
# turned four census stubs into implementations, DeleteMetaFile, PlayMetaFile,
# SetMetaFileBitsEx and StartDocA, which keep their census rows).
F5_20B_OUTSIDE = {
    'user32.dll': (
        'BeginDeferWindowPos', 'CallMsgFilterW', 'CallWindowProcW', 'CharLowerA', 'CharLowerBuffA',
        'CharNextA', 'CharPrevA', 'CharToOemA', 'CharToOemBuffA', 'CharToOemBuffW', 'CharUpperA',
        'CharUpperBuffA', 'ChildWindowFromPointEx', 'CreateIconFromResource', 'CreateWindowExW',
        'DdeClientTransaction', 'DdeConnect', 'DdeCreateDataHandle', 'DdeCreateStringHandleA',
        'DdeDisconnect', 'DdeFreeDataHandle', 'DdeFreeStringHandle', 'DdeGetData', 'DdeInitializeA',
        'DdeNameService', 'DdeQueryStringA', 'DdeUninitialize', 'DefDlgProcW', 'DefWindowProcW',
        'DeferWindowPos', 'DispatchMessageW', 'DlgDirListA', 'DlgDirListComboBoxA',
        'EndDeferWindowPos', 'FindWindowExW', 'FindWindowW', 'GetClassInfoExW', 'GetClassInfoW',
        'GetClassLongW', 'GetClassNameW', 'GetDesktopWindow', 'GetMessageW', 'GetMonitorInfoA',
        'GetPropW', 'GetTabbedTextExtentW', 'GetWindowLongW', 'GetWindowTextLengthW',
        'GetWindowTextW', 'GrayStringW', 'IsCharAlphaA', 'IsCharAlphaNumericA', 'IsCharLowerA',
        'IsCharUpperA', 'LoadCursorW', 'LoadIconW', 'LoadMenuIndirectW', 'MessageBoxExW',
        'MessageBoxIndirectA', 'MonitorFromPoint', 'OemToCharA', 'OemToCharBuffA', 'OemToCharBuffW',
        'PeekMessageW', 'PostMessageW', 'PostThreadMessageW', 'PrivateExtractIconExA',
        'PrivateExtractIconsA', 'RegisterClassExW', 'RegisterClassW', 'RegisterWindowMessageW',
        'RemovePropW', 'SendMessageTimeoutW', 'SendMessageW', 'SendNotifyMessageW', 'SetClassLongW',
        'SetPropW', 'SetWindowLongW', 'SetWindowTextW', 'SetWindowsHookExW', 'SystemParametersInfoW',
        'UnregisterClassW', 'VkKeyScanExW', 'VkKeyScanW', 'WaitForInputIdle', 'WinHelpA',
        'wsprintfA', 'wvsprintfA'),
    'gdi32.dll': ('CopyEnhMetaFileA', 'CopyMetaFileA', 'CreateMetaFileA', 'DeleteColorSpace')}
# How each f5-14B decision closed in the f5-20C build (validate requires one).
CLOSURE = {'wine-retarget': 'patches 0007/0008: imported from the Windows 98 home DLL',
           'wine-compile-in': 'patch 0008: compiled into the importing DLL',
           'wine-patch-remove': 'patch 0009: call removed',
           'w21-stub-dll': 'CiukiOS W21 stub DLL (scripts/win32/upper_dlls.py ABSENT_DELAYED)',
           'core-implement': 'core export (f5-20A/f5-20B)', 'core-w21-stub': 'core typed W21 failure',
           'w21-unresolved-forwarder': 'lazy forwarder to an absent DLL (decided)',
           'not-reused': 'importer not reused'}


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


def core_stub_status(core):
    """(dll, export) -> W21 status of core exports that are W21 stubs.

    Read from the generators' own records: census placeholders still in the
    store (generated/stubs.json), the kernel32/user32/gdi32 coverage files and
    the ADVAPI32/VERSION facade coverage."""
    core = Path(core)
    status = {}
    store = json.loads((core / 'builtin-store.json').read_text())
    hashes = {d['name'].lower(): d['sha256'] for d in store['dlls']}
    absent=core / upper_dlls.STUB_INVENTORY
    if absent.is_file():
        inventory=json.loads(absent.read_text())
        stub_dlls={d['name'] for d in store['dlls'] if d.get('origin')==upper_dlls.STUB_ORIGIN
                   and (core / d['name']).is_file() and d['sha256']==sha(core / d['name'])}
        for dll,entries in inventory['exports'].items():
            if dll not in stub_dlls:
                continue
            for row in entries:
                status[(dll,row['symbol'])]='W21 typed failure ('+row['failure']+')'
    census_manifest = core / 'generated/stubs.json'
    if census_manifest.is_file():
        reused = {m + '.dll' for m in MODULES} - set(NOT_REUSED)
        for dll in json.loads(census_manifest.read_text())['dlls']:
            image = core / 'generated' / dll['name']
            # A census placeholder that a reused Wine DLL replaces is not bound.
            if dll['name'].lower() in reused:
                continue
            if image.is_file() and hashes.get(dll['name'].lower()) == sha(image):
                for e in dll['exports']:
                    if not e['implementation']:
                        key = e['name'] or '#' + str(e['ordinal'])
                        status[(dll['name'].lower(), key)] = ('census W21 stub (' +
                                                             (e['audit'] or {}).get('failure', 'nonreturning') + ')')
    for stem in ('kernel32', 'user32', 'gdi32'):
        coverage = core / (stem + '-coverage.json')
        if coverage.is_file():
            data = json.loads(coverage.read_text())
            for row in data.get('exports', []) + data.get('upper', []):
                if 'W21' in str(row['status']):
                    status[(stem + '.dll', row['name'])] = str(row['status'])
    facades = core / 'facades-upper-coverage.json'
    if facades.is_file():
        for dll, entries in json.loads(facades.read_text()).items():
            for row in entries:
                if 'W21' in str(row['status']):
                    status[(dll, row['name'])] = str(row['status'])
    return status


def w21_bound(rows, status):
    """Reused-DLL imports and forwarders whose final hop is a core W21 stub."""
    result = []
    for importer, row in sorted(rows.items()):
        if importer in NOT_REUSED:
            continue
        for item in row['imports']:
            for symbol in item['present']:
                provider, _, name = item['resolutions'][symbol]['chain'][-1].partition('!')
                if (provider, name) in status:
                    result.append({'importer': importer, 'kind': 'delay-import' if item['delayed'] else 'import',
                                   'linked': item['dll'], 'symbol': symbol, 'provider': provider,
                                   'name': name, 'status': status[(provider, name)]})
        for forward in row['forwarders']:
            if not forward['error']:
                provider, _, name = forward['chain'][-1].partition('!')
                if (provider, name) in status:
                    result.append({'importer': importer, 'kind': 'forwarder', 'linked': importer,
                                   'symbol': '#' + str(forward['ordinal']), 'provider': provider,
                                   'name': name, 'status': status[(provider, name)]})
    return result


def core_pe_hashes(core):
    """SHA-256 of every core PE of the built SDK output (the record's core_pe_sha256)."""
    return {p.name.lower(): sha(p) for p in sorted(Path(core).iterdir())
            if p.suffix.lower() in ('.dll', '.drv', '.acm') and p.is_file()}


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
    result = {'imports': summarize(rows), 'decisions': decisions, 'core_pe_sha256': core_pe_hashes(core),
              'w21_bound': w21_bound(rows, core_stub_status(core))}
    dump(output / 'import-audit.json', rows)
    dump(output / 'decisions.json', result)
    return result


def closure(entry, rows):
    """How one f5-14B decision closed in the current build (or 'open: ...')."""
    action = entry['action']
    if action == 'not-reused':
        return 'closed: ' + CLOSURE[action]
    row = rows[entry['importer']]
    if entry['kind'] == 'forwarder':
        match = [f for f in row['forwarders'] if '#' + str(f['ordinal']) == entry['symbol']]
        if not match:
            return 'closed: forwarder removed'
        if not match[0]['error']:
            return 'closed: ' + CLOSURE[action] + ' -> ' + match[0]['chain'][-1]
        if action == 'w21-unresolved-forwarder':
            return 'closed: ' + CLOSURE[action]
        return 'open: forwarder ' + match[0]['error'] + ' ' + match[0]['chain'][-1]
    homes = {entry['linked'], entry['provider'], entry['owner'], entry['retarget']} - {None}
    found = [item['resolutions'][s] for item in row['imports'] for s in item['required']
             if s == entry['symbol'] and item['dll'] in homes]
    if not found:
        if action in ('wine-compile-in', 'wine-patch-remove'):
            return 'closed: ' + CLOSURE[action]
        return 'closed: no longer imported'
    if found[0]['error']:
        return 'open: ' + found[0]['error'] + ' ' + found[0]['chain'][-1]
    return 'closed: ' + CLOSURE[action] + ' -> ' + found[0]['chain'][-1]


def implemented_rows(rows):
    """Decision rows of the census stubs implemented in f5-20C."""
    result = []
    for row in outside_rows(rows, F5_20C_IMPLEMENTED):
        if row['importer']:
            result.append(dict(row, rule='core-api-f5-20c', audit='f5-20c', f5_20b=None,
                               f5_20c='implemented by f5-20C (lead decision 2026-10-20)'))
    for row in result:
        row.pop('f5_20b')
    return result


def outside_rows(rows, table=None):
    """Decision rows of the f5-20B exports outside the f5-14B table."""
    result = []
    for owner, names in sorted((table or F5_20B_OUTSIDE).items()):
        for name in names:
            hits = []
            for importer, row in sorted(rows.items()):
                if importer in NOT_REUSED:
                    continue
                for item in row['imports']:
                    for symbol in item['required']:
                        if item['resolutions'][symbol]['chain'][-1] == owner + '!' + name:
                            hits.append({'importer': importer, 'linked': item['dll'], 'symbol': symbol,
                                         'kind': 'delay-import' if item['delayed'] else 'import'})
                for forward in row['forwarders']:
                    if forward['chain'][-1] == owner + '!' + name:
                        hits.append({'importer': importer, 'kind': 'forwarder', 'linked': importer,
                                     'symbol': '#' + str(forward['ordinal'])})
            # Not imported by any reused DLL: the ANSI/Unicode partner row.
            for hit in hits or [{'importer': None, 'kind': 'partner', 'linked': owner, 'symbol': name}]:
                result.append({**hit, 'provider': owner, 'name': name, 'owner': owner,
                               'retarget': None, 'rule': 'core-api-f5-20b', 'action': 'core-implement',
                               'audit': 'f5-20b', 'f5_20b': 'implemented outside the decision table'})
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


def baseline_of(old):
    """The f5-14B audit, frozen: its imports, hashes and recipe (schema 1 record)."""
    if 'baseline' in old:
        return old['baseline']
    return {'audit': 'f5-14B continuation (wt/f5-14b2, 2026-10-19)', 'imports': old['imports'],
            'core_pe_sha256': old['core_pe_sha256'], 'recipe': old['recipe'], 'fit': old['fit'],
            'dlls': {n: r['sha256'] for n, r in sorted(old['dlls'].items())}}


def assemble_decisions(old_decisions, rows, current):
    """The record's decision rows: the frozen f5-14B rows with their closure
    in this build, the f5-20B outside-table rows, the re-audit's residual and
    W21-bound rows and the f5-20C implementation rows."""
    history = [dict(e, audit='f5-14b', f5_20c=closure(e, rows))
               for e in old_decisions if e.get('audit', 'f5-14b') == 'f5-14b']
    decided = {(e['importer'], e['owner'], e['name']) for e in history}
    residual = [dict(e, audit='f5-20c') for e in current['decisions']]
    bound = [{**{k: e[k] for k in ('importer', 'kind', 'linked', 'symbol', 'provider', 'name')},
              'owner': e['provider'], 'retarget': None, 'rule': 'core-census-w21',
              'action': ACTIONS['core-census-w21'], 'audit': 'f5-20c', 'f5_20c': e['status']}
             for e in current['w21_bound'] if (e['importer'], e['provider'], e['name']) not in decided]
    return history + outside_rows(rows) + residual + bound + implemented_rows(rows)


def record(output, sdk, runs):
    """Write config/wine-upper.json from a build, its identical reruns and the audit.

    The f5-14B decisions stay (audit 'f5-14b', each with its f5-20C closure);
    the f5-20B outside-table exports get rows ('f5-20b'); every import still
    missing or bound to a core W21 stub in this build gets one ('f5-20c')."""
    output = Path(output)
    build_record = json.loads((output / 'build.json').read_text())
    for other in runs:
        if json.loads(Path(other).read_text())['dlls'] != build_record['dlls']:
            raise ValueError('upper builds differ between runs: ' + str(other))
    current = json.loads((output / 'decisions.json').read_text())
    rows = json.loads((output / 'import-audit.json').read_text())
    old = json.loads(RECORD.read_text())
    surface = libc_surface(rows)
    result = {'schema': 2, 'wine_commit': build_record['wine_commit'],
              'archive_sha256': build_record['archive_sha256'],
              'recipe': {k: build_record[k] for k in ('cflags', 'ldflags', 'strip', 'base_region',
                                                      'patches', 'filters')},
              'modules': list(MODULES), 'identical_runs': 1 + len(runs),
              'dlls': {n: {**{k: r[k] for k in ('sha256', 'file_bytes', 'base', 'size_of_image',
                                                 'memory', 'license', 'qualified')},
                           'resource_entries': resource_entries((output / 'pe' / n).read_bytes())}
                       for n, r in sorted(build_record['dlls'].items())},
              'not_reused': NOT_REUSED, 'rules': RULES,
              'pending_qualification': PENDING_QUALIFICATION,
              'baseline': baseline_of(old),
              'imports': current['imports'], 'w21_bound': current['w21_bound'],
              'decisions': assemble_decisions(old['decisions'], rows, current),
              'core_pe_sha256': current['core_pe_sha256'],
              'msvcrt': msvcrt_measure(output, sdk, surface),
              'fit': fit(build_record['dlls'], NOT_REUSED),
              'status': 'built twice identically with the recorded patches; re-audited against the '
                        'core; unqualified until record-evidence qualifies the PEs whose H0 binding '
                        'and H1 DllMain evidence passes (T1 rejects the others)'}
    # Evidence of earlier PEs is not carried over: rerun the load and
    # conformance tests, then record-evidence.
    validate(result)
    dump(RECORD, result)
    return result


def qualified_by_evidence(data, name):
    """QUALIFIED names only, with H0 binding and clean H1 DllMains on this PE."""
    load = data.get('load', {})
    sha256 = data['dlls'][name]['sha256']
    h0 = load.get('h0', {}).get('dlls', {}).get(name, {})
    h1 = load.get('h1', {}).get('dlls', {}).get(name, {})
    return (data['dlls'][name]['qualified'] is True and name in QUALIFIED
            and load.get('h0', {}).get('pe_sha256', {}).get(name) == sha256
            and load.get('h1', {}).get('pe_sha256', {}).get(name) == sha256
            and h0.get('bound') is True and h1.get('outcome') == 'loaded'
            and bool(h1.get('attach')) and all(r == 1 for _, r in h1['attach'])
            and name in {n for n, _ in h1['attach']}
            and sorted(n for n, _ in h1['attach']) == sorted(n for n, _ in h1['detach']))


def record_evidence(output):
    """Copy the H0/H1 load and the conformance evidence of this build into the
    record, and qualify the QUALIFIED PEs whose evidence passes."""
    output = Path(output)
    data = json.loads(RECORD.read_text())
    evidence = {}
    for key, name in (('h0', 'h0-load.json'), ('h1', 'h1-load.json')):
        path = output / name
        if not path.is_file():
            raise ValueError('missing load evidence: ' + str(path))
        evidence[key] = json.loads(path.read_text())
        for dll, digest in evidence[key]['pe_sha256'].items():
            if data['dlls'][dll]['sha256'] != digest:
                raise ValueError(f'{name} ran on another {dll}')
    data['load'] = {**evidence, 'adapter': 'H0: native production-loader model, DllMain observed. '
                    'H1: i386 Linux adapter, production loader and core PEs, every DllMain executed; '
                    'PE ntdll with the H1 transport (identity, w32srv atoms, console), seccomp-confined.'}
    conformance = output / 'conformance.json'
    if conformance.is_file():
        data['conformance'] = json.loads(conformance.read_text())
    for name in data['dlls']:
        data['dlls'][name]['qualified'] = False
        if name in QUALIFIED:
            data['dlls'][name]['qualified'] = True
            if not qualified_by_evidence(data, name):
                data['dlls'][name]['qualified'] = False
    staged = sorted(n for n, r in data['dlls'].items() if r['qualified'])
    data['status'] = (f'built twice identically with the recorded patches; re-audited against the core; '
                      f'{len(staged)} reused PEs qualified by the recorded H0 binding and H1 DllMain '
                      'evidence and staged in the image builtin store (f5-20D); the guest case '
                      'w32-upper-dlls-qemu-t23 is the gate for their use; the others are rejected by T1')
    validate(data)
    dump(RECORD, data)
    return data


def archive_key(archive=ARCHIVE):
    """The pinned archive SHA-256 (config/wine-pin.json) and whether the file
    is present. A present archive must match the pin; nothing is downloaded."""
    pin = json.loads((ROOT / 'config/wine-pin.json').read_text())
    archive = Path(archive)
    present = archive.is_file()
    if present and sha(archive) != pin['archive_sha256']:
        raise ValueError('Wine release archive SHA-256 mismatch: ' + str(archive))
    return pin['archive_sha256'], present


def recipe_key():
    """SHA-256 of the patch set (names in order, file hashes) and of the
    recipe parameters that shape the PE bytes."""
    parts = {'patches': [[name, sha(ROOT / 'third_party/wine/patches' / name)] for name in PATCHES],
             'modules': list(MODULES), 'cflags': CFLAGS, 'ldflags': LDFLAGS,
             'base_region': [UPPER_BASE, UPPER_LIMIT], 'strip': 'llvm-objcopy --strip-all'}
    return hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest(), parts


def core_export_key(core=CORE_DIR):
    """SHA-256 of the core's export surface (every PE's ordinals, names and
    forwarders) and of its W21 stub status map: the inputs of the audit. It
    moves when an export or a stub decision changes, not when PE bytes do."""
    core = Path(core)
    surface = {name: sorted([e['ordinal'], sorted(e['names']), e['forwarder'] or '']
                            for e in meta['exports'])
               for name, (_, meta) in upper_dlls.read_images(core).items()}
    status = {dll + '!' + name: text for (dll, name), text in core_stub_status(core).items()}
    return hashlib.sha256(json.dumps({'exports': surface, 'w21': status}, sort_keys=True).encode()).hexdigest()


def pes_match(output, data=None):
    """Every PE of the cache equals the record's hash."""
    data = data or json.loads(RECORD.read_text())
    pe = Path(output) / 'pe'
    return all((pe / n).is_file() and sha(pe / n) == r['sha256'] for n, r in data['dlls'].items())


def record_differences(output, data=None):
    """Semantic differences between the cached audit and the record: an
    export now missing or present, a decision row added, removed or changed
    (including an f5-14B closure), an import whose W21 binding changed.
    Core PE hashes that merely moved are not differences."""
    data = data or json.loads(RECORD.read_text())
    output = Path(output)
    current = json.loads((output / 'decisions.json').read_text())
    rows = json.loads((output / 'import-audit.json').read_text())
    diffs = []
    for dll in sorted(set(current['imports']) | set(data['imports'])):
        now, was = current['imports'].get(dll) or {}, data['imports'].get(dll) or {}
        if now == was:
            continue
        detail = []
        for provider in sorted(set(now.get('imports', {})) | set(was.get('imports', {}))):
            a = set((now.get('imports', {}).get(provider) or {}).get('missing', []))
            b = set((was.get('imports', {}).get(provider) or {}).get('missing', []))
            if a - b:
                detail.append(provider + ' now missing ' + ', '.join(sorted(a - b)))
            if b - a:
                detail.append(provider + ' now present ' + ', '.join(sorted(b - a)))
        if now.get('forwarders_unresolved') != was.get('forwarders_unresolved'):
            detail.append('unresolved forwarders changed')
        diffs.append(dll + ': ' + ('; '.join(detail) or 'import table changed'))
    key = lambda e: (e['audit'], e['importer'] or '', e['kind'], e['linked'], e['symbol'])
    expected = {key(e): e for e in assemble_decisions(data['decisions'], rows, current)}
    recorded = {key(e): e for e in data['decisions']}
    for k in sorted(set(expected) | set(recorded)):
        if expected.get(k) != recorded.get(k):
            what = ('decision removed' if k not in expected else 'decision added' if k not in recorded
                    else 'decision changed')
            e = expected.get(k) or recorded.get(k)
            closure_text = e.get('f5_20c')
            diffs.append(f'{what}: {k[1] or k[3]} {k[2]} {k[3]}!{k[4]} ({e["rule"]}' +
                         (f', {closure_text}' if closure_text else '') + ')')
    if current['w21_bound'] != data['w21_bound']:
        now = {(e['importer'], e['linked'], e['symbol']) for e in current['w21_bound']}
        was = {(e['importer'], e['linked'], e['symbol']) for e in data['w21_bound']}
        for importer, linked, symbol in sorted(now - was):
            diffs.append(f'{importer}: {linked}!{symbol} now bound to a core W21 stub')
        for importer, linked, symbol in sorted(was - now):
            diffs.append(f'{importer}: {linked}!{symbol} no longer bound to a core W21 stub')
        if now == was:
            diffs.append('W21-bound import statuses changed')
    return diffs


def refresh_core_hashes(core, log=print):
    """The record's core-DLL hashes follow the built core (the audit found
    no semantic difference); returns how many moved. Read from the core
    itself: a cached audit may predate a rebuild that moved only PE bytes."""
    data = json.loads(RECORD.read_text())
    current = core_pe_hashes(core)
    moved = sorted(n for n in set(data['core_pe_sha256']) | set(current)
                   if data['core_pe_sha256'].get(n) != current.get(n))
    if moved:
        data['core_pe_sha256'] = current
        validate(data)
        dump(RECORD, data)
        log('[upper] core-DLL hashes refreshed in config/wine-upper.json: ' + ', '.join(moved))
    return len(moved)


def ensure_audit(output, core, cache, actions):
    """The cached audit is current for this core, or a new one runs; returns
    (builtin store hash, core export key)."""
    output, core = Path(output), Path(core)
    # The manifest alone is insufficient for `check`: a rebuilt PE or a
    # changed coverage decision may precede regeneration of that manifest.
    # Hash the actual inputs before deciding that the cached audit is current.
    statuses = {dll + '!' + name: status for (dll, name), status in core_stub_status(core).items()}
    store_sha = hashlib.sha256(json.dumps({
        'manifest': sha(core / 'builtin-store.json'), 'pes': core_pe_hashes(core),
        'w21': statuses}, sort_keys=True).encode()).hexdigest()
    files = all((output / n).is_file() for n in ('decisions.json', 'import-audit.json'))
    if files and cache.get('core_store_sha256') == store_sha:
        return store_sha, cache['core_export_sha256']
    export_sha = core_export_key(core)
    if files and cache.get('core_export_sha256') == export_sha:
        actions.append('core PE hashes moved without an export change: cached audit kept')
    else:
        actions.append(('core exports changed: re-auditing' if cache.get('core_export_sha256') else 'auditing')
                       + ' the reused DLLs against the core')
        audit(output, core)
    return store_sha, export_sha


def write_cache(output, archive_sha, recipe_sha, recipe, store_sha, export_sha, actions):
    pe = {n + '.dll': sha(Path(output) / 'pe' / (n + '.dll')) for n in MODULES}
    dump(Path(output) / 'cache.json',
         {'schema': 1, 'archive_sha256': archive_sha, 'recipe_sha256': recipe_sha, 'recipe': recipe,
          'core_store_sha256': store_sha, 'core_export_sha256': export_sha, 'dlls': pe,
          'last_run': {'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'actions': actions}})


def cached_build(output=CACHE, core=CORE_DIR, archive=ARCHIVE, keep=False, log=print):
    """The make build-full step (make upper): PEs built once per (archive,
    patch set and recipe), audited once per core export surface, the record's
    core-DLL hashes refreshed; a build that does not reproduce the record or
    an audit that differs from its decisions is refused. Never downloads.
    Returns the actions taken (each logged with its reason)."""
    output, core, archive = Path(output), Path(core), Path(archive)
    data = json.loads(RECORD.read_text())
    store = core / 'builtin-store.json'
    if not store.is_file():
        raise ValueError('core builtins not built (run make apps first): ' + str(store))
    cache_path = output / 'cache.json'
    cache = json.loads(cache_path.read_text()) if cache_path.is_file() else {}
    archive_sha, present = archive_key(archive)
    recipe_sha, recipe = recipe_key()
    actions = []
    current = (cache.get('archive_sha256') == archive_sha and cache.get('recipe_sha256') == recipe_sha
               and pes_match(output, data))
    if not current:
        reason = ('no cached upper build' if not cache else
                  'Wine archive hash changed' if cache.get('archive_sha256') != archive_sha else
                  'patch set or recipe changed' if cache.get('recipe_sha256') != recipe_sha else
                  'cached PEs missing or changed')
        build_json = output / 'build.json'
        built = json.loads(build_json.read_text()) if build_json.is_file() else {}
        if (built.get('archive_sha256') == archive_sha and built.get('patches') == dict(recipe['patches'])
                and built.get('cflags') == CFLAGS and built.get('ldflags') == LDFLAGS
                and built.get('base_region') == recipe['base_region'] and built.get('strip') == recipe['strip']
                and pes_match(output, data)):
            actions.append(reason + ': the recipe output already present is adopted')
        else:
            if not present:
                raise ValueError(reason + ', and the pinned Wine archive is absent (' + str(archive) +
                                 '); the build never downloads')
            log('[upper] ' + reason + ': rebuilding the reused Wine DLLs')
            started = time.monotonic()
            build(output, archive)
            actions.append(f'{reason}: rebuilt {len(MODULES)} PEs in {time.monotonic() - started:.0f} s')
        if not keep:
            for name in ('source', 'objects'):
                shutil.rmtree(output / name, ignore_errors=True)
        wrong = sorted(n for n, r in data['dlls'].items() if sha(output / 'pe' / n) != r['sha256'])
        if wrong:
            raise ValueError('the upper build does not reproduce config/wine-upper.json: ' + ', '.join(wrong) +
                             ' (pinned toolchain changed, or the record needs wine_upper.py record '
                             'after two identical builds)')
        cache = {}
    store_sha, export_sha = ensure_audit(output, core, cache, actions)
    diffs = record_differences(output, data)
    if diffs:
        raise ValueError('the reused Wine DLL audit differs from config/wine-upper.json:\n  ' +
                         '\n  '.join(diffs) +
                         '\nreview the core change, then run scripts/win32/wine_upper.py refresh-audit')
    moved = refresh_core_hashes(core, log)
    if moved:
        actions.append(f'core-DLL hashes refreshed in config/wine-upper.json ({moved} moved)')
    if not actions:
        actions.append('cached upper build reused (archive, patch set and core exports unchanged)')
    write_cache(output, archive_sha, recipe_sha, recipe, store_sha, export_sha, actions)
    for action in actions:
        log('[upper] ' + action)
    return actions


def check(output=CACHE, core=CORE_DIR, log=print):
    """The record is self-consistent and rendered; with the built core and the
    cached PEs present, the audit is compared with the record (fails only on
    a semantic difference) and the core-DLL hashes are refreshed."""
    data = json.loads(RECORD.read_text())
    validate(data)
    if render(data) not in DOCUMENT.read_text():
        raise ValueError('generated upper DLL tables differ from the record')
    output, core = Path(output), Path(core)
    if (output / 'pe').is_dir() and not pes_match(output, data):
        raise ValueError('cached upper PEs missing or changed: run make upper')
    if not (core / 'builtin-store.json').is_file() or not (output / 'pe').is_dir():
        log('PASS reused Wine upper DLL record (offline: built core or cached PEs absent; make upper audits them)')
        return 0
    cache_path = output / 'cache.json'
    cache = json.loads(cache_path.read_text()) if cache_path.is_file() else {}
    actions = []
    store_sha, export_sha = ensure_audit(output, core, cache, actions)
    diffs = record_differences(output, data)
    if diffs:
        raise ValueError('the reused Wine DLL audit differs from config/wine-upper.json:\n  ' +
                         '\n  '.join(diffs) +
                         '\nreview the core change, then run scripts/win32/wine_upper.py refresh-audit')
    moved = refresh_core_hashes(core, log)
    if cache:
        write_cache(output, cache['archive_sha256'], cache['recipe_sha256'], cache['recipe'], store_sha, export_sha,
                    actions + [f'check: {moved} core-DLL hashes refreshed'])
    log(f'PASS reused Wine upper DLL record (audited against the built core; {moved} core-DLL hashes refreshed)')
    return moved


def refresh_audit(output=CACHE, core=CORE_DIR, log=print):
    """After a reviewed core change: the audit-derived parts of the record
    (imports, W21 bindings, decision rows and closures, core hashes) from the
    cached PEs and the built core; the PE build, the evidence and the
    measurements stay. A changed MSVCRT import surface needs `record`."""
    output, core = Path(output), Path(core)
    data = json.loads(RECORD.read_text())
    if not pes_match(output, data):
        raise ValueError('cached PEs absent or different from the record: run make upper')
    current = audit(output, core)
    rows = json.loads((output / 'import-audit.json').read_text())
    if libc_surface(rows) != data['msvcrt']['surface']:
        raise ValueError('the MSVCRT import surface changed: rerun wine_upper.py record (SDK measurement)')
    before = len(data['decisions'])
    data['decisions'] = assemble_decisions(data['decisions'], rows, current)
    data['imports'], data['w21_bound'] = current['imports'], current['w21_bound']
    data['core_pe_sha256'] = current['core_pe_sha256']
    validate(data)
    dump(RECORD, data)
    write_document(data)
    cache_path = output / 'cache.json'
    if cache_path.is_file():
        cache = json.loads(cache_path.read_text())
        write_cache(output, cache['archive_sha256'], cache['recipe_sha256'], cache['recipe'],
                    sha(core / 'builtin-store.json'), core_export_key(core),
                    ['audit-derived record parts refreshed (refresh-audit)'])
    log(f'[upper] record refreshed from the cached audit: {before} -> {len(data["decisions"])} decision rows, '
        f'{len(data["w21_bound"])} W21-bound imports')
    return data


def _missing_keys(imports):
    keys = set()
    for dll, row in imports.items():
        for provider, item in row['imports'].items():
            kind = 'delay-import' if provider.endswith(' (delayed)') else 'import'
            for symbol in item['missing']:
                keys.add((dll, kind, provider.removesuffix(' (delayed)'), symbol))
    return keys


def validate(data, patches=ROOT / 'third_party/wine/patches'):
    """Self-consistency of the committed record (offline; no build needed)."""
    if data.get('schema') != 2:
        raise ValueError('record schema 2 expected')
    if list(data['recipe']['patches']) != list(PATCHES):
        raise ValueError('patch order differs from the recipe')
    for name, digest in data['recipe']['patches'].items():
        if sha(Path(patches) / name) != digest:
            raise ValueError('patch hash mismatch: ' + name)
    if set(data['recipe']['filters']) != {p for p in PATCHES if not p.endswith('.patch')}:
        raise ValueError('recipe filters differ from the patch list')
    if set(data['dlls']) != {m + '.dll' for m in MODULES}:
        raise ValueError('built module set differs from the recipe')
    if data.get('pending_qualification') != PENDING_QUALIFICATION:
        raise ValueError('pending qualification differs from the reviewed candidates')
    low, high = data['recipe']['base_region']
    spans = sorted((r['base'], r['base'] + r['size_of_image'], n) for n, r in data['dlls'].items())
    for (start, end, name), following in zip(spans, spans[1:] + [(high, high, '')]):
        if start < low or start & 0xffff or end > following[0]:
            raise ValueError('reused DLL base outside region or overlapping: ' + name)
        if data['dlls'][name]['qualified'] is not False and not qualified_by_evidence(data, name):
            raise ValueError('reused DLL marked qualified: ' + name)
        if data['dlls'][name]['license'] != 'LGPL-2.1-or-later':
            raise ValueError('reused DLL licence: ' + name)
    seen = set()
    for entry in data['decisions']:
        key = (entry['audit'], entry['importer'], entry['kind'], entry['linked'], entry['symbol'])
        if key in seen:
            raise ValueError('duplicate decision: ' + repr(key))
        seen.add(key)
        if entry['rule'] not in RULES or ACTIONS[entry['rule']] != entry['action']:
            raise ValueError('unknown decision rule/action: ' + repr(key))
        if entry['audit'] not in ('f5-14b', 'f5-20b', 'f5-20c'):
            raise ValueError('decision without a known audit: ' + repr(key))
    by_audit = lambda name: [e for e in data['decisions'] if e['audit'] == name]
    # f5-14B: one decision per missing entry of the frozen baseline audit.
    baseline = data['baseline']['imports']
    for dll, row in baseline.items():
        if len(row['forwarders_unresolved']) != sum(
                1 for e in by_audit('f5-14b') if e['importer'] == dll and e['kind'] == 'forwarder'):
            raise ValueError('forwarder decisions differ from the audit: ' + dll)
    if _missing_keys(baseline) != {k[1:] for k in seen if k[0] == 'f5-14b' and k[2] != 'forwarder'}:
        raise ValueError('missing exports without exactly one decision')
    for entry in by_audit('f5-14b'):
        if not str(entry.get('f5_20c', '')).startswith('closed: '):
            raise ValueError('f5-14B decision not closed: ' + repr((entry['importer'], entry['symbol'])))
    # f5-20B outside-table exports: every listed name has a row, nothing else.
    outside = {(e['owner'], e['name']) for e in by_audit('f5-20b')}
    if outside != {(o, n) for o, names in F5_20B_OUTSIDE.items() for n in names}:
        raise ValueError('f5-20B outside-table rows differ from the list')
    # f5-20C re-audit: one decision per missing entry and per W21-bound import.
    current = data['imports']
    for dll, row in current.items():
        if len(row['forwarders_unresolved']) != sum(
                1 for e in by_audit('f5-20c') if e['importer'] == dll and e['kind'] == 'forwarder'
                and e['rule'] not in ('core-census-w21', 'core-api-f5-20c')):
            raise ValueError('re-audit forwarder decisions differ: ' + dll)
    residual = {k[1:] for k in seen if k[0] == 'f5-20c' and k[2] != 'forwarder'} - {
        (e['importer'], e['kind'], e['linked'], e['symbol']) for e in by_audit('f5-20c')
        if e['rule'] in ('core-census-w21', 'core-api-f5-20c')}
    for entry in by_audit('f5-20c'):
        if entry['rule'] == 'core-api-f5-20c' and entry['name'] not in F5_20C_IMPLEMENTED.get(entry['owner'], ()):
            raise ValueError('f5-20C implementation row outside the list: ' + entry['name'])
    if _missing_keys(current) != residual:
        raise ValueError('re-audit missing exports without exactly one decision')
    decided = {(e['importer'], e['owner'], e['name']) for e in by_audit('f5-14b')}
    bound = {(e['importer'], e['kind'], e['linked'], e['symbol']) for e in by_audit('f5-20c')
             if e['rule'] == 'core-census-w21'}
    for entry in data['w21_bound']:
        if ((entry['importer'], entry['provider'], entry['name']) not in decided and
                (entry['importer'], entry['kind'], entry['linked'], entry['symbol']) not in bound):
            raise ValueError('W21-bound import without a decision: ' + repr(entry))
    return True


# Upstream Wine 10.0 conformance units run on the i386 adapter (f5-20C):
# the units that exercise the f5-02 census functions of each DLL (provisional
# research union: comctl32 ImageList_*, comdlg32 Find/Replace/Print/Open/Save,
# shell32 Drag*/SH*/ShellExecute*/Shell_NotifyIcon, ole32 Co*/Ole* init);
# riched20 has no census function: its windowless text services unit.
CONFORMANCE = {
    'comctl32': ('imagelist',),
    'comdlg32': ('finddlg', 'printdlg', 'filedlg'),
    'shell32': ('shellole', 'shlfolder', 'shellpath', 'shlfileop', 'shelllink', 'shlexec',
                'brsfolder', 'systray'),
    'ole32': ('compobj', 'ole2'),
    'riched20': ('txtsrv',),
}


def conformance_build(output):
    """Build the upstream test programs of CONFORMANCE from the patched
    source of an upper build, with test-runtime stubs.

    The programs import a few functions the CiukiOS core lacks (the test
    framework's GetProcessId, test-only USER/GDI calls). Per the contract,
    test-runtime imports do not define F5 scope: each one is satisfied inside
    the program by a stub failing with ERROR_CALL_NOT_IMPLEMENTED, recorded
    per program, so the program binds; the DLL under test is unchanged."""
    output = Path(output).resolve()
    source, work = output / 'source', output / 'tests-objects'
    if not (source / 'configure').is_file():
        raise ValueError('no patched source; run the upper build first')
    providers = output / 'providers'
    if not providers.is_dir():
        raise ValueError('no providers; run the upper audit first')
    out = output / 'tests-pe'
    out.mkdir(exist_ok=True)
    with (output / 'tests-build.log').open('w') as log:
        if not (work / 'Makefile').is_file():
            work.mkdir(exist_ok=True)
            run(['../source/configure', '--enable-archs=i386', '--disable-win16', '--disable-build-id',
                 '--with-mingw=clang', '--without-x', '--without-wayland', '--without-freetype',
                 '--without-fontconfig', '--without-vulkan', '--without-opengl', '--without-gnutls',
                 'CC=clang', 'CXX=clang++', 'i386_CFLAGS=' + CFLAGS, 'i386_LDFLAGS=' + LDFLAGS], work, log)
    images = upper_dlls.read_images(providers)
    index = {n: upper_dlls.export_index(meta) for n, (_, meta) in images.items()}
    result = {}
    for module in CONFORMANCE:
        target = f'dlls/{module}/tests/i386-windows/{module}_test.exe'
        logfile = output / f'tests-{module}.log'
        # Relinked every time so that the log carries the link command.
        (work / target).unlink(missing_ok=True)
        with logfile.open('w') as log:
            run(['make', '-j1', target], work, log)
        exe = work / target
        pe = census.PE(exe.read_bytes())
        missing = sorted({(item['dll'].lower(), s) for delayed in (False, True) for item in pe.imports(delayed)
                          for s in set(item['symbols'])
                          if upper_dlls.resolve(index, item['dll'].lower(), s)['error']})
        # Decorated import symbols from the import libraries the program links.
        stubs_c = ['/* Generated by scripts/win32/wine_upper.py: test-runtime stubs. */',
                   '__declspec(dllimport) void __stdcall SetLastError(unsigned);']
        names = []
        for i, (dll, name) in enumerate(missing):
            lib = work / f'dlls/{dll.rsplit(".", 1)[0]}/i386-windows/lib{dll.rsplit(".", 1)[0]}.a'
            nm = subprocess.run(['llvm-nm', lib], capture_output=True, text=True, check=True).stdout
            found = re.search(r'__imp__' + re.escape(name) + r'@(\d+)\b', nm)
            if not found:
                raise ValueError(f'no stdcall import symbol for {dll}!{name}')
            words = int(found[1]) // 4
            args = ', '.join(f'unsigned a{j}' for j in range(words)) or 'void'
            unused = ''.join(f'(void)a{j}; ' for j in range(words))
            # Both the import pointer and the direct-call thunk name, so that
            # the import library member is never fetched.
            stubs_c.append(f'unsigned __stdcall stub{i}({args}) __asm__("_{name}@{found[1]}");')
            stubs_c.append(f'unsigned __stdcall stub{i}({args}) {{ {unused}SetLastError(120); return 0; }}')
            stubs_c.append(f'void *imp{i} __asm__("__imp__{name}@{found[1]}") = (void *)stub{i};')
            names.append(f'{dll}!{name}')
        stub_source = out / f'{module}_runtime.c'
        stub_source.write_text('\n'.join(stubs_c) + '\n')
        stub_object = out / f'{module}_runtime.o'
        with logfile.open('a') as log:
            run(['clang', '--target=i686-windows', '-O1', '-c', stub_source, '-o', stub_object], work, log)
        # The program's own link command (makefile log), plus the stubs.
        text = logfile.read_text().replace('\\\n', ' ')
        line = next(l for l in text.splitlines() if l.startswith('tools/winegcc/winegcc -o ' + target))
        args = line.split()
        args[args.index('-o') + 1] = str(out / (module + '_test-linked.exe'))
        first_library = next(i for i, a in enumerate(args) if a.endswith('.a'))
        args.insert(first_library, str(stub_object))
        with logfile.open('a') as log:
            run(args, work, log)
            run(['llvm-objcopy', '--strip-all', out / (module + '_test-linked.exe'),
                 out / (module + '_test.exe')], work, log)
        (out / (module + '_test-linked.exe')).unlink()
        result[module] = {'program': module + '_test.exe', 'sha256': sha(out / (module + '_test.exe')),
                          'units': list(CONFORMANCE[module]), 'runtime_stubs': names}
    dump(out / 'programs.json', result)
    return result


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
        use = ('measured, not reused' if name in data['not_reused'] else
               'reused, qualified (staged builtin)' if r['qualified'] else
               'build candidate, pending H1' if name in data.get('pending_qualification', {}) else
               'reused (unqualified)')
        out.append(f"| {name} | `{r['base']:#010x}` | {r['file_bytes']:,} | {r['size_of_image']:,} | "
                   f"{r['memory']['shared_seed_pages']} | {r['memory']['private_cow_commitment_pages']} | "
                   f"{r['resource_entries']:,} | `{r['sha256'][:16]}` | {use} |")
    for name, reason in sorted(data.get('pending_qualification', {}).items()):
        out += ['', f'`{name}`: {reason}.']
    out += ['', '| Patch (applied in order) | SHA-256 (prefix) |', '| --- | --- |']
    out += [f'| `{name}` | `{digest[:16]}` |' for name, digest in data['recipe']['patches'].items()]
    for name, f in sorted(data['recipe']['filters'].items()):
        out += ['', f"`{name}` (resource filter): {f['images_before']} icon images "
                    f"({f['bytes_before']:,} bytes) in, {f['images_after']} ({f['bytes_after']:,} bytes) out."]
    out += ['', '### Import re-audit against the f5-20A/B core (generated)', '',
            'Missing eager imports, delayed imports and unresolved export forwarders of the current '
            'build, and imports whose final hop is a core W21 stub (typed failure); each has a '
            'decision (`audit` = `f5-20c`, or its f5-14B row).', '',
            '| DLL | Eager missing | Delayed missing | Unresolved forwarders | Bound to W21 stubs |',
            '| --- | ---: | ---: | ---: | ---: |']
    bound = Counter(e['importer'] for e in data['w21_bound'])
    for name, row in data['imports'].items():
        delayed = sum(len(i['missing']) for k, i in row['imports'].items() if k.endswith(' (delayed)'))
        out.append(f"| {name} | {row['eager_missing']} | {delayed} | "
                   f"{len(row['forwarders_unresolved'])} | {bound.get(name, 0)} |")
    out += ['', '| Audit | Action | Entries |', '| --- | --- | ---: |']
    counts = Counter((e['audit'], e['action']) for e in data['decisions'])
    out += [f'| {a} | `{c}` | {n} |' for (a, c), n in sorted(counts.items())]
    closed = Counter(e['f5_20c'].split(' -> ')[0] for e in data['decisions'] if e['audit'] == 'f5-14b')
    out += ['', 'Closure of the f5-14B decisions in this build:', '', '| Closure | Entries |', '| --- | ---: |']
    out += [f'| {c} | {n} |' for c, n in sorted(closed.items())]
    groups = defaultdict(set)
    for e in data['decisions']:
        groups[(e['audit'], e['rule'], e['owner'])].add(e['name'])
    out += ['', '| Rule | Action | Decision |', '| --- | --- | --- |']
    rules = sorted({e['rule'] for e in data['decisions']})
    out += [f"| `{r}` | `{ACTIONS[r]}` | {data['rules'][r]} |" for r in rules]
    out += ['', 'Unique names per audit, rule and owning DLL:', '']
    for (audit_name, rule, owner), names in sorted(groups.items()):
        out.append(f"- {audit_name} `{rule}` → {owner} ({len(names)}): " + ', '.join(sorted(names)))
    load = data.get('load')
    if load:
        out += ['', '### Load and DllMain evidence (generated)', '', load['adapter'], '',
                '| DLL | H0 binding | H0 DllMain attach/detach (observed) | H1 outcome | '
                'H1 DllMain attach TRUE | H1 detach | Kernel records ENOSYS | w32srv requests |',
                '| --- | --- | ---: | --- | ---: | ---: | ---: | ---: |']
        for name in sorted(set(load['h0']['dlls']) | set(load['h1']['dlls'])):
            h0 = load['h0']['dlls'].get(name, {})
            h1 = load['h1']['dlls'].get(name)
            if h1 is None:
                out.append(f"| {name} | {'bound' if h0.get('bound') else 'refused'} | "
                           f"{h0.get('dllmain_attach_observed', 0)}/{h0.get('dllmain_detach_observed', 0)} | "
                           'not executed (sandbox SIGSYS) | - | - | - | - |')
                continue
            res = dict(re.findall(r'(\w+)=(\d+)', h1.get('resources', '')))
            out.append(f"| {name} | {'bound' if h0['bound'] else 'refused'} | "
                       f"{h0['dllmain_attach_observed']}/{h0['dllmain_detach_observed']} | {h1['outcome']} | "
                       f"{sum(1 for _, v in h1['attach'] if v == 1)}/{len(h1['attach'])} | "
                       f"{len(h1['detach'])} | {res.get('kernel_enosys', '-')} | {res.get('server_requests', '-')} |")
    conformance = data.get('conformance')
    if conformance:
        out += ['', '### Upstream conformance units on the H1 adapter (generated)', '',
                'Wine 10.0 `dlls/*/tests` programs (test-runtime imports missing from the core are '
                'stubbed inside the program, listed below); the adapter has no CiukiOS kernel, so '
                'kernel record operations it does not model fail with ENOSYS.', '',
                '| Unit | Outcome | Executed | Failures | Skipped | Failed checks reported | ENOSYS kernel operations |',
                '| --- | --- | ---: | ---: | ---: | ---: | --- |']
        for unit, r in sorted(conformance['units'].items()):
            ops = ', '.join(f'{k}×{v}' for k, v in sorted(r['kernel_enosys'].items())) or '-'
            out.append(f"| {unit} | {r['outcome']}{' ' + r['exception'] if r.get('exception') else ''} | "
                       f"{r.get('executed', '-')} | {r.get('failures', '-')} | {r.get('skipped', '-')} | "
                       f"{r['failed_checks_reported']} | {ops} |")
        out += ['', 'Test-runtime stubs per program: ' + '; '.join(
            f"{m}: {', '.join(p['runtime_stubs']) or 'none'}" for m, p in sorted(conformance['programs'].items()))]
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
    f, b = data['fit'], data['baseline']['fit']
    yes = lambda v: 'yes' if v else 'no'
    out += ['', '### 128 MiB fit figures (generated)', '',
            '| Figure | f5-20C build | f5-14B build |', '| --- | ---: | ---: |',
            f"| Reused DLLs | {len(f['reused_dlls'])} | {len(b['reused_dlls'])} |",
            f"| File bytes | {f['file_bytes']:,} | {b['file_bytes']:,} |",
            f"| SizeOfImage | {f['size_of_image']:,} | {b['size_of_image']:,} |",
            f"| Shared seed pages | {f['shared_seed_pages']} ({f['shared_seed_pages'] * 4096:,} bytes) | "
            f"{b['shared_seed_pages']} |",
            f"| Private COW commitment pages | {f['private_cow_commitment_pages']} | "
            f"{b['private_cow_commitment_pages']} |",
            f"| Startup committed pages (model) | {f['startup_committed_pages_model']} | "
            f"{b['startup_committed_pages_model']} |",
            f"| Contract installer budget (P2+P4) | {f['installer_budget_bytes']:,} | {b['installer_budget_bytes']:,} |",
            f"| SizeOfImage within budget | {yes(f['size_of_image_within_budget'])} | "
            f"{yes(b['size_of_image_within_budget'])} |",
            f"| SHELL32 SizeOfImage | {f['shell32_size_of_image']:,} | {b['shell32_size_of_image']:,} |", '',
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
    p = sub.add_parser('check', help='validate the record; with a built core and cached PEs, audit and refresh hashes')
    p.add_argument('--output', type=Path, default=CACHE)
    p.add_argument('--core', type=Path, default=CORE_DIR)
    sub.add_parser('render')
    p = sub.add_parser('cached-build', help='the make build-full step: build once per key, audit, refresh hashes')
    p.add_argument('--output', type=Path, default=CACHE)
    p.add_argument('--core', type=Path, default=CORE_DIR)
    p.add_argument('--archive', type=Path, default=ARCHIVE)
    p.add_argument('--keep-intermediates', action='store_true', help='keep source/ and objects/ (conformance-build)')
    p = sub.add_parser('refresh-audit', help='regenerate the audit-derived record parts from the cached PEs')
    p.add_argument('--output', type=Path, default=CACHE)
    p.add_argument('--core', type=Path, default=CORE_DIR)
    p = sub.add_parser('conformance-build', help='upstream test programs for the H1 adapter')
    p.add_argument('output', type=Path)
    p = sub.add_parser('record-evidence', help='load/DllMain/conformance evidence into the record')
    p.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.command == 'build':
        build(args.output, args.archive)
    elif args.command == 'audit':
        audit(args.output, args.core)
    elif args.command == 'record':
        record(args.output, args.sdk, args.runs)
    elif args.command == 'conformance-build':
        print(json.dumps(conformance_build(args.output), indent=2))
    elif args.command == 'record-evidence':
        data = record_evidence(args.output)
        write_document(data)
        print('qualified:', sorted(n for n, r in data['dlls'].items() if r['qualified']))
    elif args.command == 'render':
        data = json.loads(RECORD.read_text())
        validate(data)
        write_document(data)
    elif args.command == 'cached-build':
        cached_build(args.output, args.core, args.archive, args.keep_intermediates)
    elif args.command == 'refresh-audit':
        refresh_audit(args.output, args.core)
    else:
        check(args.output, args.core)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f'wine upper: {exc}')
