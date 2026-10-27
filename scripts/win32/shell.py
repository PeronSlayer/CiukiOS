#!/usr/bin/env python3
"""Build platform-owned shell entry points beside the reused Wine shell32.

The loader's explicit forwarding set applies to both the census placeholder
and the qualified reused DLL. Other Wine exports and provenance stay intact.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
from pathlib import Path
import re
import subprocess
from scripts.win32 import census, codecs
ROOT = Path(__file__).resolve().parents[2]
KERNEL = dict(codecs.KERNEL, SetLastError=4, GetLastError=0,
    CreateDirectoryA=8, RemoveDirectoryA=4, MoveFileA=8, CopyFileA=12,
    DeleteFileA=4, GetFileAttributesA=4, GetFullPathNameA=16,
    FindFirstFileA=8, FindNextFileA=8, FindClose=4, CreateProcessA=40,
    MultiByteToWideChar=24, WriteFile=20, FlushFileBuffers=4,
    LoadLibraryExA=12, FreeLibrary=4, FindResourceA=12,
    LoadResource=8, SizeofResource=8)
KERNEL.update(GlobalLock=4, GlobalUnlock=4, GlobalSize=4, GlobalFree=4,
    HeapReAlloc=16, HeapSize=12,
    GetDriveTypeA=4, GetSystemTime=4, SystemTimeToFileTime=8, Sleep=4)   # G2: SHFileOperation's Recycle Bin

def build(out, base, flags):
    out = Path(out)
    objdir = out/'shell-objects'
    objdir.mkdir(parents=True, exist_ok=True)
    libraries = [codecs.import_library(objdir, 'kernel32', KERNEL),
        codecs.import_library(objdir, 'user32', {'MessageBoxA': 16, 'CiukiShellNotify': 12,
            'PrivateExtractIconExA': 20, 'PrivateExtractIconsA': 32,
            'DestroyIcon': 4, 'GetWindowLongA': 8, 'SetWindowLongA': 12,
            'GetPropA':8,'SetPropA':12,'RemovePropA':8,'DefWindowProcA':16,
            'RegisterClassA':4,'CreateWindowExA':48,'ShowWindow':8,'UpdateWindow':4,
            'SetWindowTextA':8,'PeekMessageA':20,'PostQuitMessage':4,
            'TranslateMessage':4,'DispatchMessageA':4,'DestroyWindow':4,
            'IsWindow':4,'CiukiShellOpenFolder':8}),   # fx-q3vise: ShellExecute(Ex) of a folder
        codecs.import_library(objdir, 'advapi32', {'RegOpenKeyExA': 20,
            'RegQueryValueExA': 24, 'RegCloseKey': 4})]
    # cdecl diagnostic functions are exported by ntdll with plain names.
    definition = objdir/'ntdll.def'
    definition.write_text('LIBRARY ntdll.dll\nEXPORTS\n w32_diag\n w32_report_refusal\n')
    lib = objdir/'ntdll.lib'
    codecs.run(['llvm-dlltool', '-m', 'i386', '-d', definition, '-l', lib])
    libraries.append(lib)
    objects, exports = [], {}
    for source in [*sorted((ROOT/'sdk/win32/shell').glob('*.c')),
                   ROOT/'sdk/win32/services/bytes.c', ROOT/'sdk/win32/services/stack.S']:
        obj = objdir/(source.stem+'.obj')
        extra = ['-Wno-unused-command-line-argument'] if source.suffix == '.S' else []
        codecs.run(['clang', *flags, *extra, '-c', source, '-o', obj])
        objects.append(obj)
        nm = subprocess.run(['llvm-nm', '--defined-only', str(obj)],
            capture_output=True, text=True, check=True).stdout
        exports.update({name: f'_{name}@{arity}' for name,arity in
            re.findall(r'\bT _([A-Za-z][A-Za-z0-9_]*)@(\d+)\s*$', nm, re.M)})
    definition = objdir/'ciukishell.def'
    definition.write_text('LIBRARY ciukishell.dll\nEXPORTS\n'+''.join(
        f' {n}={body}\n' for n,body in sorted(exports.items())))
    image = out/'ciukishell.dll'
    resource_source = objdir/'version.rc'
    resource_source.write_text('1 VERSIONINFO\nFILEVERSION 0,0,0,0\nPRODUCTVERSION 0,0,0,0\n'
        'FILEOS 0x40004\nFILETYPE 2\nBEGIN\nBLOCK "StringFileInfo"\nBEGIN\n'
        'BLOCK "040904E4"\nBEGIN\nVALUE "ProductName", "CiukiOS\\0"\nEND\nEND\n'
        'BLOCK "VarFileInfo"\nBEGIN\nVALUE "Translation", 0x409, 1252\nEND\nEND\n')
    resource = objdir/'version.res'
    codecs.run(['llvm-rc','/fo',resource,resource_source])
    subprocess.run(list(map(str,['lld-link', '/machine:x86', '/dll', '/noentry', '/nodefaultlib',
        '/safeseh:no', '/base:'+hex(base), '/timestamp:0', '/incremental:no',
        '/subsystem:windows,4.0', '/osversion:4.0', '/def:'+str(definition),
        '/out:'+str(image), *objects, resource, *libraries])),check=True)
    raw = image.read_bytes()
    return {'name': image.name, 'path': image.name, 'base': base,
        'size': census.PE(raw).size, 'sha256': hashlib.sha256(raw).hexdigest(),
        'version': [0,0,0,0]}
