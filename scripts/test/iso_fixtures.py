#!/usr/bin/env python3
"""Deterministic, lawful data CDs; independent xorriso extraction manifests.

No boot image build or copies. Scratch trees live under the caller's build/
run directory and are removed before returning. Manifest paths are the names
in the chosen primary/Joliet hierarchy, independently extracted by xorriso.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import argparse
import struct

DATE = '2020010100000000'
EPOCH = 1577836800

def generate_directory(source, output, joliet=True):
    """Python-only extension of the f1-58 directory-record fixtures."""
    from iso_directory import build
    return build(source, output, joliet)

def generate_mixed_mode(source, directory, audio_seconds=4):
    """Authored data + CD-DA media; needs a track-aware guest transport."""
    from iso_directory import mixed_mode
    return mixed_mode(source, directory, audio_seconds)

# ---- fx-acc1: InstallShield 5 test medium -------------------------------------
# A medium the IS5 native bootstrap (docs/design/installshield5-bootstrap.md,
# src/kernel/proc/win32/is5boot.h, sdk/win32/is5boot) admits and runs, made of
# authored bytes only (no commercial InstallShield or Stirling code):
# SETUP.EXE is a minimal Windows NE header (admission reads only the header),
# SETUP.INS is a marker, SETUP.INI and SETUP.LID carry the keys the bootstrap
# reads, and _INST32I.EX_ is a Stirling installer archive (82-byte header,
# signature 2a ab 79 d8 00 01, member directory; Deark installshld.c) whose
# three members are literal-only PKWARE DCL streams (zlib contrib/blast:
# literal flag 0, 6 dictionary bits, each byte a 0 bit then its 8 bits, the
# end code length 519). INSTALL.EXE is the authored engine below; the two
# support DLLs are marker files the bootstrap only writes and deletes.
IS5_ENGINE_LOG = 'C:\\Documents\\is5engine.txt'
IS5_ENGINE_EXIT = 42
IS5_SIGNATURE = bytes((0x2a, 0xab, 0x79, 0xd8, 0x00, 0x01, 0x00, 0x00))


def is5_dcl_literal(data):
    """Literal-only DCL (blast): header bytes 0 (uncoded literals) and 6; per
    byte a 0 bit and its 8 bits; the end is length code 519 (a 1 bit, the
    7-bit code of length symbol 15, 255 in its 8 extra bits), LSB first."""
    bits = []
    for byte in data:
        bits.append(0)
        bits += [(byte >> i) & 1 for i in range(8)]
    bits += [1] + [0] * 7 + [1] * 8
    out = bytearray([0, 6]) + bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        out[2 + i // 8] |= bit << (i % 8)
    return bytes(out)


def is5_archive(members):
    """Stirling installer archive; members are (name, runtime name, bytes)."""
    directory = bytearray()
    packed = [is5_dcl_literal(data) for _, _, data in members]
    offset = 82 + sum(20 + 4 + len(name) + len(runtime) for name, runtime, _ in members)
    for (name, runtime, data), stream in zip(members, packed):
        directory += struct.pack('<III8x', offset, len(stream), len(data))
        directory += struct.pack('<H', len(name)) + name.encode('ascii')
        directory += struct.pack('<H', len(runtime)) + runtime.encode('ascii')
        offset += len(stream)
    header = bytearray(82)
    header[0:8] = IS5_SIGNATURE
    struct.pack_into('<I', header, 78, len(members))
    return bytes(header + directory + b''.join(packed))


def is5_ne_stub():
    """A 256-byte Windows NE application header, no code: MZ with e_lfanew
    0x80, "NE", target OS 2 (Windows), application flags (bit 15 clear), an
    empty resource table (the bootstrap then uses its English error texts)."""
    stub = bytearray(256)
    stub[0:2] = b'MZ'
    struct.pack_into('<I', stub, 60, 0x80)
    stub[0x80:0x82] = b'NE'
    struct.pack_into('<H', stub, 0x80 + 36, 64)        # resource table at NE+64: shift 4, no types
    struct.pack_into('<H', stub, 0x80 + 64, 4)
    stub[0x80 + 54] = 2
    return bytes(stub)


def is5_files():
    """The root files of the InstallShield 5 test medium."""
    return {'SETUP.EXE': is5_ne_stub(),
            'SETUP.INS': b'CiukiOS acceptance InstallShield 5 test script marker\r\n',
            'SETUP.INI': b'[Startup]\r\nAppName=CiukiOS IS5 Test\r\nEnableLangDlg=N\r\n',
            'SETUP.LID': b'[Languages]\r\nDefault=0009\r\n',
            '_INST32I.EX_': is5_archive([('INSTALL.EXE', '_INS0000._MP', is5_engine_pe()),
                                         ('zdata.dll', 'ZDataI51.dll', b'CiukiOS IS5 test support file 1\r\n'),
                                         ('WUTL95i.DLL', '_WUTL951.DLL', b'CiukiOS IS5 test support file 2\r\n')])}


def is5_engine_pe(log_path=None, exit_code=None):
    """fx-acc1: the stand-in InstallShield engine (INSTALL.EXE of the test
    archive), an authored 1.5 KiB PE32 GUI program written byte by byte (no
    toolchain, no third-party code). It writes the current directory it was
    started in (the bootstrap starts the engine from the media root) to
    log_path and exits with exit_code; it imports CreateFileA,
    GetCurrentDirectoryA, WriteFile, CloseHandle and ExitProcess from
    kernel32.dll (Microsoft PE/COFF specification: optional header, import
    directory and IAT). Checked on the host under Wine."""
    log_path = log_path or IS5_ENGINE_LOG
    exit_code = IS5_ENGINE_EXIT if exit_code is None else exit_code
    base, rva = 0x400000, 0x1000
    names = ['CreateFileA', 'GetCurrentDirectoryA', 'WriteFile', 'CloseHandle', 'ExitProcess']
    sec = bytearray(0x400)
    # import descriptor at 0x000 (+ null at 0x014); ILT at 0x028; IAT at 0x040
    ilt, iat, dll = 0x028, 0x040, 0x058
    sec[dll:dll + 13] = b'kernel32.dll\0'
    at = 0x068
    hints = []
    for name in names:
        hints.append(at)
        entry = b'\0\0' + name.encode() + b'\0'
        if len(entry) % 2: entry += b'\0'
        sec[at:at + len(entry)] = entry; at += len(entry)
    for k, h in enumerate(hints):
        struct.pack_into('<I', sec, ilt + 4 * k, rva + h)
        struct.pack_into('<I', sec, iat + 4 * k, rva + h)
    struct.pack_into('<IIIII', sec, 0, rva + ilt, 0, 0, rva + dll, rva + iat)
    path = log_path.encode('ascii') + b'\0'
    path_at = at; sec[at:at + len(path)] = path; at += len(path)
    at = (at + 15) & ~15
    written, buf = 0x2d8, 0x2e0   # zero-filled; buf + 260 stays inside the 0x400-byte section
    va = lambda off: base + rva + off
    imp = lambda name: va(iat + 4 * names.index(name))
    code = bytearray()
    code += b'\x6a\x00' + b'\x68' + struct.pack('<I', 0x80) + b'\x6a\x02' + b'\x6a\x00' + b'\x6a\x00'
    code += b'\x68' + struct.pack('<I', 0x40000000) + b'\x68' + struct.pack('<I', va(path_at))
    code += b'\xff\x15' + struct.pack('<I', imp('CreateFileA'))
    code += b'\x89\xc6'                                         # mov esi, eax
    code += b'\x68' + struct.pack('<I', va(buf)) + b'\x68' + struct.pack('<I', 260)
    code += b'\xff\x15' + struct.pack('<I', imp('GetCurrentDirectoryA'))
    code += b'\x6a\x00' + b'\x68' + struct.pack('<I', va(written)) + b'\x50'
    code += b'\x68' + struct.pack('<I', va(buf)) + b'\x56'
    code += b'\xff\x15' + struct.pack('<I', imp('WriteFile'))
    code += b'\x56' + b'\xff\x15' + struct.pack('<I', imp('CloseHandle'))
    code += b'\x6a' + bytes([exit_code]) + b'\xff\x15' + struct.pack('<I', imp('ExitProcess'))
    entry = at; sec[at:at + len(code)] = code; at += len(code)
    assert at <= written
    head = bytearray(0x200)
    head[0:2] = b'MZ'; struct.pack_into('<I', head, 60, 0x40)
    head[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', head, 0x44, 0x14c, 1, 0, 0, 0, 0xe0, 0x0103)
    o = 0x58
    struct.pack_into('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', head, o,
                     0x10b, 1, 0, 0x400, 0, 0, rva + entry, rva, rva, base, 0x1000, 0x200,
                     4, 0, 0, 0, 4, 0, 0, 0x2000, 0x200, 0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    dirs = o + 96
    struct.pack_into('<II', head, dirs + 8 * 1, rva, 40)          # import table
    struct.pack_into('<II', head, dirs + 8 * 12, rva + iat, 24)   # IAT
    s = 0x58 + 0xe0
    head[s:s + 8] = b'.text\0\0\0'
    struct.pack_into('<IIIIIIHHI', head, s + 8, 0x400, rva, 0x400, 0x200, 0, 0, 0, 0, 0xe0000060)
    return bytes(head + sec)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def command(args):
    p = subprocess.run(args, capture_output=True, text=True, env=dict(os.environ, LC_ALL='C.UTF-8', TZ='UTC'))
    if p.returncode:
        raise ValueError('ISO fixture command failed: ' + ' '.join(map(str, args)) + '\n' + p.stderr[-3000:])
    return p

def descriptor_offsets(image):
    """Byte offsets of the primary and supplementary volume descriptors.

    ECMA-119 8.1/8.4/8.5: the volume descriptor set starts at logical sector
    16 (2048-byte sectors); type 1 is the primary, type 2 a supplementary
    (Joliet) descriptor, type 255 the terminator. Bytes 1..5 are "CD001"."""
    offsets = []
    with Path(image).open('rb') as stream:
        for sector in range(16, 16 + 32):
            stream.seek(sector * 2048)
            head = stream.read(7)
            if len(head) < 7 or head[1:6] != b'CD001':
                raise ValueError('ISO volume descriptor set is not terminated')
            if head[0] == 255:
                return offsets
            if head[0] in (1, 2):
                offsets.append(sector * 2048)
    raise ValueError('ISO volume descriptor set exceeds 32 sectors')


def set_volume_sequence(image, number):
    """Write the Volume Sequence Number (ECMA-119 8.4.13, BP 125-128: a both-byte
    order 16-bit field, little- then big-endian) of every primary and
    supplementary descriptor. Mastering tools always write 1; discs pressed with
    0 there exist (fx-accept: the T23 refused one as malformed_iso9660)."""
    if type(number) is not int or not 0 <= number <= 0xFFFF:
        raise ValueError('volume sequence number must be 0..65535')
    patched = []
    with Path(image).open('r+b') as stream:
        for offset in descriptor_offsets(image):
            stream.seek(offset + 124)
            stream.write(number.to_bytes(2, 'little') + number.to_bytes(2, 'big'))
            patched.append(offset // 2048)
    return patched


def root_entries(entries):
    """Names directly below the root of an extraction manifest."""
    return sorted(e['path'] for e in entries if e['path'].count('/') == 1)


def generate(spec, directory, index=0, media_name=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    level = spec.get('level', 3)
    if level not in (1, 2, 3) or spec.get('seed', 1) != 1:
        raise ValueError('unsupported ISO level/seed')
    label = spec.get('label', 'CIUKI_F4')
    if not isinstance(label, str) or not 1 <= len(label) <= 32 or not all(c.isupper() or c.isdigit() or c == '_' for c in label):
        raise ValueError('ISO volume label must be 1..32 characters A-Z, 0-9 or _')
    if media_name is not None and (not isinstance(media_name, str) or not media_name.replace('-', '').isalnum()):
        raise ValueError('ISO fixture name must be letters, digits and dashes')
    stem = media_name or str(index)
    source = directory / f'iso-source-{stem}'
    extracted = directory / f'iso-extracted-{stem}'
    image = directory / (f'media-{media_name}.iso' if media_name else f'fixture-{index}.iso')
    if source.exists() or extracted.exists():
        raise ValueError('ISO scratch directory already exists')
    content = spec.get('content')
    if content not in (None, 'is5'):
        raise ValueError('ISO content: is5 or the default fixture tree')
    files = {'README.TXT': b'Ciuki F4 optical fixture seed=1\n',
             'DATA/ODD.BIN': bytes((i * 17 + 3) & 255 for i in range(4099)),
             'DATA/EMPTY.BIN': b''}
    if content == 'is5':
        files = {'README.TXT': b'CiukiOS InstallShield 5 bootstrap test medium (authored bytes only)\n', **is5_files()}
    if level > 1 and content is None:
        files['Long directory/Long optical file.bin'] = bytes(range(256)) * 33
    if spec.get('joliet') and content is None:
        files['Caf\u00e9/Gr\u00fc\u00dfe.txt'] = 'Ciuki: caff\u00e8, Gr\u00fc\u00dfe.\n'.encode()
    try:
        source.mkdir()
        for path, data in sorted(files.items()):
            p = source / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        for p in [*source.rglob('*'), source]:
            os.utime(p, (EPOCH, EPOCH))
        args = ['xorriso', '-as', 'mkisofs', '-iso-level', str(level),
                '--norock', '-V', label, '--modification-date=' + DATE,
                '--set_all_file_dates', DATE, '-uid', '0', '-gid', '0']
        if spec.get('joliet'):
            args += ['-J']
        args += ['-o', str(image), str(source)]
        command(args)
        extracted.mkdir()
        result = command(['xorriso', '-osirrox', 'on', '-indev', str(image), '-extract', '/', str(extracted)])
        entries = []
        for p in sorted(extracted.rglob('*')):
            name = '/' + p.relative_to(extracted).as_posix()
            entries.append({'path': name, 'path_hex': name.encode().hex(),
                            'kind': 'directory' if p.is_dir() else 'file',
                            **({} if p.is_dir() else {'size': p.stat().st_size, 'sha256': sha(p)})})
        manifest = {'generator': 'xorriso', 'level': level, 'joliet': bool(spec.get('joliet')),
                    **({'content': content} if content else {}),
                    'seed': 1, 'path': str(image.resolve()), 'size': image.stat().st_size,
                    'declared_files': {p: hashlib.sha256(b).hexdigest() for p, b in sorted(files.items())},
                    'entries': entries, 'independent_tool': 'xorriso -osirrox on -extract /',
                    'tool_version': command(['xorriso', '-version']).stdout,
                    'extract_output': (result.stdout + result.stderr)[-3000:]}
        manifest['label'] = label
        manifest['root_entries'] = root_entries(entries)
        if spec.get('volume_sequence') is not None:
            manifest['volume_sequence'] = spec['volume_sequence']
            manifest['volume_sequence_sectors'] = set_volume_sequence(image, spec['volume_sequence'])
        if spec.get('malformed'):
            # Broken root extent, with agreeing little/big-endian encodings.
            with image.open('r+b') as f:
                f.seek(16 * 2048 + 158)
                extent = image.stat().st_size // 2048 + 1
                f.write(extent.to_bytes(4, 'little') + extent.to_bytes(4, 'big'))
            manifest['malformed'] = 'root_extent_outside_volume'
        manifest['sha256'] = sha(image)
        (directory / (f'media-{media_name}-manifest.json' if media_name else f'fixture-{index}-manifest.json')).write_text(json.dumps(manifest, indent=2) + '\n')
        return manifest
    finally:
        for tree in (source,extracted):
            if tree.exists():
                # ISO directories are read-only after independent extraction.
                for p in [tree,*tree.rglob('*')]:
                    if p.is_dir():p.chmod(0o700)
                shutil.rmtree(tree)

def verify_records(manifest, records):
    entries = [r for r in records if r.get('case') == 'entry']
    expected = manifest['entries']
    if manifest.get('malformed'):
        if entries:
            raise ValueError('malformed ISO exposed entries')
        if not any(r.get('case') == 'malformed' and r.get('refused') == '1' for r in records):
            raise ValueError('malformed ISO was not refused')
        return
    for cycle in ('1', '2'):
        actual = [r for r in entries if r.get('cycle') == cycle]
        if len(actual) != len(expected) or len({r['path_hex'] for r in actual}) != len(actual):
            raise ValueError('optical directory listing differs from independent manifest')
        by_path = {r['path_hex']: r for r in actual}
        for entry in expected:
            r = by_path.get(entry['path_hex'])
            if not r or r.get('kind') != entry['kind']:
                raise ValueError('optical name/type differs from independent manifest')
            if entry['kind']=='file':
                hashes=[h for h in records if h.get('case')=='file' and h.get('cycle')==cycle and h.get('id')==r.get('id')]
                if len(hashes)!=1 or hashes[0].get('sha256')!=entry['sha256'] or hashes[0].get('size')!=str(entry['size']):
                    raise ValueError('optical file hash/size differs from independent manifest')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--level', type=int, choices=(1, 2, 3), default=3)
    parser.add_argument('--joliet', action='store_true')
    parser.add_argument('--malformed', action='store_true')
    parser.add_argument('--volume-sequence', type=int, dest='volume_sequence',
                        help='Volume Sequence Number written into every primary/supplementary descriptor')
    parser.add_argument('--source', type=Path, help='directory for Python-only mastering')
    parser.add_argument('--audio-seconds', type=int, help='authored mixed-mode CUE/PCM fixture')
    args = parser.parse_args()
    if args.audio_seconds is not None:
        if args.source is None: parser.error('--audio-seconds requires --source')
        result = generate_mixed_mode(args.source, args.out, args.audio_seconds)
    elif args.source is not None:
        if args.malformed: parser.error('--source does not support --malformed')
        result = generate_directory(args.source, args.out/'fixture.iso', args.joliet)
    else:
        result = generate(vars(args), args.out)
    print(json.dumps(result, indent=2))
