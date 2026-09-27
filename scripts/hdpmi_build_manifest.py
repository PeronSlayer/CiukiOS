#!/usr/bin/env python3
"""Record exact HDPMI build inputs and reject changes during compilation."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('begin', 'finish'))
    for name in ('root', 'hx', 'output', 'xms-patch', 'session-patch',
                 'adapter', 'jwasm', 'jwlink', 'wcc', 'wlib', 'tools'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--modules', nargs='+', required=True)
    args = parser.parse_args()
    root, hx, out = args.root.resolve(), args.hx.resolve(), args.output.resolve()
    inputs = {}
    def add(label, path):
        inputs[label] = {'path': str(path.resolve()), 'sha256': sha(path)}
    for module in args.modules:
        add('HX/Src/HDPMI/' + module + '.ASM', hx / 'Src/HDPMI' / (module + '.ASM'))
    for directory in (hx / 'Src/HDPMI', hx / 'Include'):
        # These are the two explicit assembler include directories. Symlink
        # aliases resolve to the same original bytes and are not extra inputs.
        for path in sorted(directory.iterdir()):
            if path.is_file() and not path.is_symlink() and path.suffix.lower() == '.inc':
                add('HX/' + str(path.relative_to(hx)), path)
    names = ('hdpmi_video_adapter.c', 'hdpmi_video_adapter.h', 'vga_x86.c',
             'vga_x86.h', 'virtual_vga.c', 'virtual_vga.h',
             'session_scheduler_abi.inc', 'session_scheduler.h',
             'session_video_abi.inc', 'CIUKIOS-HDP324-MODIFICATIONS.TXT')
    for name in names:
        add('CiukiOS/src/vm/' + name, root / 'src/vm' / name)
    for name in ('build_hdpmi_host.sh', 'hdpmi_build_manifest.py'):
        add('CiukiOS/scripts/' + name, root / 'scripts' / name)
    for name, path in (('xms_patch', args.xms_patch), ('session_patch', args.session_patch),
                       ('adapter', args.adapter), ('JWasm', args.jwasm),
                       ('JWlink', args.jwlink), ('wcc386', args.wcc), ('wlib', args.wlib),
                       ('EDITPE', args.tools / 'EDITPE.EXE'),
                       ('PESTUB', args.tools / 'PESTUB.EXE')):
        add(name, path)
    snapshot = {
        'hx_commit': subprocess.check_output(['git', '-C', str(hx), 'rev-parse', 'HEAD'], text=True).strip(),
        'inputs': inputs,
        'assembler_defines': ['?32BIT=1/0', '?PMIOPL=0', '?PE', '?WDEB386=1',
                              '?JHDPMI=1', '?EMUDRxRD=1', '?EMUDRxWR=1'],
        'c_flags': ['-bt=nt', '-mf', '-3r', '-ecc', '-zl', '-zc', '-s', '-ox', '-os'],
    }
    pending = out / 'HDPMI-BUILD-INPUTS.json'
    if args.phase == 'begin':
        pending.write_text(json.dumps(snapshot, indent=2) + '\n')
        return
    if json.loads(pending.read_text()) != snapshot:
        raise SystemExit('HDPMI inputs changed during build; outputs are not qualified')
    binaries = {}
    for name in ('HDPMI32I.EXE', 'HDPMI16I.EXE'):
        path = out / name
        data = path.read_bytes()
        if len(data) < 64 or data[:2] != b'MZ':
            raise SystemExit('Invalid HDPMI output: ' + name)
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        if pe + 24 > len(data) or data[pe:pe+4] not in (b'PE\0\0', b'PX\0\0'):
            raise SystemExit('Missing HDPMI executable payload: ' + name)
        count = struct.unpack_from('<H', data, pe + 6)[0]
        if count != 3:
            raise SystemExit('HDPMI payload must retain exactly three sections: ' + name)
        binaries[name] = {'bytes': len(data), 'sha256': sha(path), 'sections': count}
    manifest = dict(snapshot, binaries=binaries, build_complete=True,
                    runtime_tested=False, inputs_unchanged_during_build=True)
    (out / 'HDPMI-BUILD.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('[build-hdpmi] exact source/tool/output manifest: ' + str(out / 'HDPMI-BUILD.json'))


if __name__ == '__main__':
    main()
