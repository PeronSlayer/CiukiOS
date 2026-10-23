"""Offline ACPICA inputs and checked, generated platform/parser adapters."""
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
VENDOR = ROOT / 'third_party/acpica'

def prepare(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)  # lead fix 2026-10-23: canonical build starts from an empty obj dir
    manifest = json.loads((HERE / 'manifest.json').read_text())
    actual = {str(p.relative_to(VENDOR)) for p in VENDOR.rglob('*') if p.is_file()}
    expected = {row['path'] for row in manifest['files']}
    audit = (ROOT / 'docs/design/peripheral-driver-source-audit.md').read_text().split('## ACPICA 20260930 runtime')[1].split('## Alternative implementations')[0]
    audited = {p: (blob, verdict == 'A2L') for p, verdict, blob in re.findall(
        r'\| \[`([^`]+)`\].*?\| (A2L?) \| \d+ \| `([0-9a-f]{40})`', audit)}
    if len(audited) != 207 or expected != set(audited) or any(
            (row['blob'], row['legacy']) != audited[row['path']] for row in manifest['files']):
        raise ValueError('ACPICA pin differs from the reviewed source audit')
    if actual != expected:
        raise ValueError('ACPICA vendor set differs from the audited manifest')
    sources = []
    for row in manifest['files']:
        p = VENDOR / row['path']
        data = p.read_bytes()
        sha = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if sha != row['blob']:
            raise ValueError(f'ACPICA blob mismatch: {p}')
        if p.suffix == '.h':
            dst = out / 'include' / p.relative_to(VENDOR / 'source/include')
            dst.parent.mkdir(parents=True, exist_ok=True)
            if p.name == 'acenv.h':
                old = b'#if defined(_LINUX) || defined(__linux__)'
                assert data.count(old) == 1
                data = data.replace(old, b'#if defined(CIUKI_ACPICA)\n#include "ciuki_env.h"\n#elif defined(_LINUX) || defined(__linux__)')
            if p.name == 'acpi.h':
                data += b'\n#include "seam.h"\n'
            dst.write_bytes(data)
        elif p.name == 'evregion.c':
            old = b'*RegNamePtr, Node, ACPI_TYPE_METHOD, &MethodNode)'
            assert data.count(old) == 1
            data = data.replace(old, b'AcpiCiukiName32 ((const char *) RegNamePtr), Node, ACPI_TYPE_METHOD, &MethodNode)')
            dst = out / 'evregion.c'
            dst.write_bytes(data)
            sources.append(dst)
        elif p.name == 'exmutex.c':
            old = b'    if (ObjDesc == AcpiGbl_GlobalLockMutex)\n    {\n'
            assert data.count(old) == 2
            # No fake success for firmware global-lock dependent AML. The
            # sticky error also blocks Field I/O whose upstream helper is void.
            data = data.replace(old, old + b'        (void) AcpiCiukiParseResult (AE_SUPPORT);\n        return_ACPI_STATUS (AE_SUPPORT);\n', 1)
            dst = out / 'exmutex.c'
            dst.write_bytes(data)
            sources.append(dst)
        elif p.name == 'nsaccess.c':
            old = b'*ACPI_CAST_PTR (UINT32, InitVal->Name)'
            assert data.count(old) == 1
            dst = out / 'nsaccess.c'
            dst.write_bytes(data.replace(old, b'AcpiCiukiName32 (InitVal->Name)'))
            sources.append(dst)
        elif p.name == 'psargs.c':
            start = data.index(b'static UINT32\nAcpiPsGetNextPackageLength (', data.index(b'FUNCTION:    AcpiPsGetNextPackageLength'))
            end = data.index(b'\n/*******************************************************************************', start)
            part = data[start:end]
            assert part.count(b'return_UINT32 (0);') == 2
            part = part.replace(b'return_UINT32 (0);', b'(void) AcpiCiukiParseResult (AE_AML_PACKAGE_LIMIT);\n        return_UINT32 (0);')
            data = data[:start] + part + data[end:]
            old = b'    return_PTR (Start + PackageLength); /* end of package */'
            assert data.count(old) == 1
            data = data.replace(old, b'''    if (PackageLength < (ACPI_SIZE) (ParserState->Aml - Start) ||
        PackageLength > (ACPI_SIZE) (ParserState->AmlEnd - Start))
    {
        (void) AcpiCiukiParseResult (AE_AML_PACKAGE_LIMIT);
        return_PTR (ParserState->AmlEnd);
    }
''' + old)
            dst = out / 'psargs.c'
            dst.write_bytes(data)
            sources.append(dst)
        elif p.name == 'psloop.c':
            old = b'    while ((ParserState->Aml < ParserState->AmlEnd) || (Op))\n    {\n'
            assert data.count(old) == 1
            hook = b'''        /* Ciuki: common budget for module AML and every nested method. */
        Status = AcpiCiukiBudgetStep ();
        if (ACPI_FAILURE (Status))
        {
            (void) AcpiPsCompleteOp (WalkState, &Op, Status);
            return_ACPI_STATUS (Status);
        }
'''
            dst = out / 'psloop.c'
            data = data.replace(old, old + hook)
            marker = b'#define _COMPONENT'
            assert data.count(marker) == 1
            # Preserve parser errors even when AcpiLoadTables deliberately
            # continues after a malformed individual definition block.
            data = data.replace(marker, b'#undef return_ACPI_STATUS\n#define return_ACPI_STATUS(s) return (AcpiCiukiParseResult(s))\n\n' + marker)
            dst.write_bytes(data)
            sources.append(dst)
        elif p.name not in ('rsdump.c', 'rsdumpinfo.c'):
            # Upstream debugger-only resource dump is not guarded as a whole
            # and refers to debug-only tables. Retain its audited bytes, but
            # do not compile debugger code in a kernel without the debugger.
            sources.append(p)
    return sources

def flags(out):
    # Namespace the optional upstream C library instead of overriding the
    # kernel/host libc. The same functions are compiled on both targets.
    clib = (VENDOR / 'source/include/acclib.h').read_text()
    names = re.findall(r'^([a-z][a-z0-9_]*)\s*\(', clib, re.M)
    return ['-DCIUKI_ACPICA', '-U__linux__', '-U_LINUX', '-I', str(Path(out) / 'include'), '-I', str(HERE),
            '-I', str(ROOT / 'src/kernel/include'),
            *[f'-D{n}=AcpiCiuki_{n}' for n in names]]
