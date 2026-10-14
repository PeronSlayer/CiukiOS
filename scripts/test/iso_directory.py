"""Python-only ISO 9660 level 1 + Joliet directory fixture mastering.

Encoding extends f1-58 root-record fixtures: fixed 2020-01-01 dates, 2048-byte
blocks, both-endian fields, full path tables, separate directory hierarchies.
Payloads stream from disk. No boot, Rock Ridge, multi-extent or multisession.
"""
from pathlib import Path
import hashlib
import re
import stat
import struct

BLOCK = 2048
DATE = bytes((120, 1, 1, 0, 0, 0, 0))
VOLUME_DATE = b'2020010100000000\0'


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def both(value, width=4):
    return value.to_bytes(width, 'little')+value.to_bytes(width, 'big')


def record(extent, size, name, directory=False):
    length = 33+len(name)+(len(name) % 2 == 0)
    if length > 255:
        raise ValueError('directory record exceeds 255 bytes')
    r = bytearray(length)
    r[0] = length
    r[2:10] = both(extent)
    r[10:18] = both(size)
    r[18:25] = DATE
    r[25] = 2 if directory else 0
    r[28:32] = both(1, 2)
    r[32] = len(name)
    r[33:33+len(name)] = name
    return bytes(r)


def pack_records(records):
    data = bytearray()
    for r in records:
        used = len(data) % BLOCK
        if used+len(r) > BLOCK:
            data.extend(bytes(BLOCK-used))
        data.extend(r)
    data.extend(bytes((-len(data)) % BLOCK))
    return bytes(data)


def scan(source):
    source = Path(source)
    if source.is_symlink() or not source.is_dir():
        raise ValueError('source must be a real directory')
    nodes = {'': {'path': source, 'parent': '', 'directory': True, 'children': []}}
    pending = ['']
    directories = []
    while pending:
        parent = pending.pop(0)
        directories.append(parent)
        siblings = sorted(nodes[parent]['path'].iterdir(), key=lambda p: p.name.encode('utf-8'))
        names = set()
        aliases = set()
        for index, path in enumerate(siblings, 1):
            name = path.name
            if (len(name) > 64 or any(ord(c) < 32 or ord(c) > 0xffff or 0xd800 <= ord(c) <= 0xdfff for c in name)
                    or any(c in name for c in '/\\;') or name.endswith((' ', '.'))):
                raise ValueError('unsupported Joliet name: '+name)
            if name.casefold() in names:
                raise ValueError('case-insensitive Joliet name collision')
            names.add(name.casefold())
            mode = path.lstat().st_mode
            directory = stat.S_ISDIR(mode)
            if not directory and not stat.S_ISREG(mode):
                raise ValueError('symlink/special source file refused')
            if not directory and len(name) > 62:
                raise ValueError('Joliet file identifier including ;1 exceeds 64 characters')
            relative = path.relative_to(source).as_posix()
            if len(Path(relative).parts) > (7 if directory else 8):
                raise ValueError('ISO level 1 depth exceeds eight')
            upper = name.upper()
            pattern = r'[A-Z0-9_]{1,8}' if directory else r'[A-Z0-9_]{1,8}(?:\.[A-Z0-9_]{1,3})?'
            alias = upper if re.fullmatch(pattern, upper) else ('D' if directory else 'F')+f'{index:07d}'+('' if directory else '.BIN')
            if alias in aliases:
                raise ValueError('primary alias collision')
            aliases.add(alias)
            size = 0 if directory else path.stat().st_size
            if size > 0xffffffff:
                raise ValueError('multi-extent payload not supported')
            nodes[relative] = {'path': path, 'parent': parent, 'directory': directory,
                               'children': [], 'size': size, 'primary': alias, 'joliet': name}
            nodes[parent]['children'].append(relative)
            if directory:
                pending.append(relative)
    if len(directories) > 65535:
        raise ValueError('too many directories')
    return nodes, directories


def name_bytes(node, joliet):
    name = node['joliet' if joliet else 'primary']
    if not node['directory']:
        if not joliet and '.' not in name:
            name += '.'
        name += ';1'
    return name.encode('utf-16-be' if joliet else 'ascii')


def directory_bytes(key, nodes, layout, joliet):
    node = nodes[key]
    parent = node['parent']
    extent, size = layout[key]
    pextent, psize = layout[parent]
    records = [record(extent, size, b'\0', True), record(pextent, psize, b'\1', True)]
    for child in sorted(node['children'], key=lambda k: name_bytes(nodes[k], joliet)):
        item = nodes[child]
        cextent, csize = layout[child] if item['directory'] else (item.get('extent', 0), item['size'])
        records.append(record(cextent, csize, name_bytes(item, joliet), item['directory']))
    return pack_records(records)


def path_table(nodes, directories, layout, joliet, endian):
    ids = {name: i+1 for i, name in enumerate(directories)}
    result = bytearray()
    for key in directories:
        name = name_bytes(nodes[key], joliet) if key else b'\0'
        result.extend(bytes((len(name), 0)))
        result.extend(layout[key][0].to_bytes(4, endian))
        result.extend(ids[nodes[key]['parent']].to_bytes(2, endian))
        result.extend(name)
        if len(name) % 2:
            result.append(0)
    return bytes(result)


def build(source, output, joliet=True):
    """Return a manifest usable by OpticalHarness.check_image; no external tools."""
    nodes, directories = scan(source)
    output = Path(output)
    if output.resolve().is_relative_to(Path(source).resolve()):
        raise ValueError('output cannot live inside source')
    hierarchies = [False, True] if joliet else [False]
    layouts = {}
    tables = {}
    next_lba = 17+len(hierarchies)  # descriptor terminator is before this
    for unicode in hierarchies:
        ordered = ['']
        for parent in ordered:
            ordered.extend(sorted((key for key in nodes[parent]['children'] if nodes[key]['directory']),
                                  key=lambda key: name_bytes(nodes[key], unicode)))
        layout = {key: (0, 0) for key in ordered}
        lengths = {key: len(directory_bytes(key, nodes, layout, unicode)) for key in ordered}
        length = len(path_table(nodes, ordered, layout, unicode, 'little'))
        table_blocks = (length+BLOCK-1)//BLOCK
        tables[unicode] = (next_lba, next_lba+table_blocks, length)
        next_lba += 2*table_blocks
        for key in ordered:
            layout[key] = (next_lba, lengths[key])
            next_lba += lengths[key]//BLOCK
        layouts[unicode] = (layout, ordered)
    for key, node in sorted(nodes.items()):
        if not node['directory']:
            node['extent'] = next_lba if node['size'] else 0
            next_lba += (node['size']+BLOCK-1)//BLOCK
    total = max(300, next_lba)  # at least four seconds for the data track
    if total > 0xffffffff:
        raise ValueError('volume too large')
    output.parent.mkdir(parents=True, exist_ok=True)
    entries = []
    with output.open('wb') as stream:
        stream.truncate(total*BLOCK)
        for i, unicode in enumerate(hierarchies):
            b = bytearray(BLOCK)
            b[:7] = bytes((2 if unicode else 1,))+b'CD001\1'
            b[8:40] = ('CIUKIOS'.ljust(16).encode('utf-16-be') if unicode else b'CIUKIOS'.ljust(32, b' '))
            b[40:72] = ('CIUKI_FIXTURE'.ljust(16).encode('utf-16-be') if unicode else b'CIUKI_FIXTURE'.ljust(32, b' '))
            b[80:88] = both(total)
            if unicode:
                b[88:91] = b'%/E'
            b[120:124] = both(1, 2)
            b[124:128] = both(1, 2)
            b[128:132] = both(BLOCK, 2)
            le, be, length = tables[unicode]
            b[132:140] = both(length)
            b[140:144] = le.to_bytes(4, 'little')
            b[148:152] = be.to_bytes(4, 'big')
            layout, ordered = layouts[unicode]
            b[156:190] = record(*layout[''], b'\0', True)
            b[190:702] = (b'\0 ' if unicode else b'  ')*256
            b[702:813] = b' '*111
            b[813:830] = VOLUME_DATE
            b[830:847] = VOLUME_DATE
            b[847:864] = b'0'*16+b'\0'
            b[864:881] = VOLUME_DATE
            b[881] = 1
            stream.seek((16+i)*BLOCK)
            stream.write(b)
            for endian, extent in (('little', le), ('big', be)):
                stream.seek(extent*BLOCK)
                stream.write(path_table(nodes, ordered, layout, unicode, endian))
            for key in ordered:
                stream.seek(layout[key][0]*BLOCK)
                stream.write(directory_bytes(key, nodes, layout, unicode))
        stream.seek((16+len(hierarchies))*BLOCK)
        stream.write(b'\xffCD001\1'+bytes(BLOCK-7))
        for key, node in sorted(nodes.items()):
            if node['directory']:
                continue
            stream.seek(node['extent']*BLOCK)
            h = hashlib.sha256()
            count = 0
            with node['path'].open('rb') as payload:
                for chunk in iter(lambda: payload.read(1024*1024), b''):
                    count += len(chunk)
                    if count > node['size']:
                        raise ValueError('source changed during mastering')
                    stream.write(chunk)
                    h.update(chunk)
            if count != node['size']:
                raise ValueError('source changed during mastering')
            node['sha256'] = h.hexdigest()
    for key, node in sorted(nodes.items()):
        if not key:
            continue
        parts = key.split('/')
        selected = '/'+key if joliet else '/'+ '/'.join(nodes['/'.join(parts[:i+1])]['primary'] for i in range(len(parts)))
        entries.append({'path': selected, 'path_hex': selected.encode().hex(),
                        'kind': 'directory' if node['directory'] else 'file',
                        **({} if node['directory'] else {'size': node['size'], 'sha256': node['sha256']})})
    return {'generator': 'python-iso9660', 'level': 1, 'joliet': joliet, 'path': str(output.resolve()),
            'size': total*BLOCK, 'sha256': sha(output), 'entries': entries, 'session': 0}


def mixed_mode(source, directory, audio_seconds=4):
    """Multi-file CUE, cooked data + project-authored S16LE stereo test tone.

    A 150-sector silent pregap is in the audio file (INDEX 00), followed by
    at least 300 AUDIO sectors. Never pretend these bytes are ordinary ISO.
    """
    if type(audio_seconds) is not int or not 4 <= audio_seconds <= 60:
        raise ValueError('audio fixture duration must be 4..60 integral seconds')
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    data = build(source, directory/'data.iso')
    audio = directory/'audio.bin'
    # Integer square wave: repeatable on every Python/platform, no FP sine.
    tone = b''.join(struct.pack('<hh', 6000 if (i//50)%2 else -6000,
                              6000 if (i//50)%2 else -6000) for i in range(58800))
    with audio.open('wb') as stream:
        stream.write(bytes(150*2352))
        sectors = audio_seconds*75
        for i in range(sectors):
            at = (i*2352) % len(tone)
            stream.write(tone[at:at+2352])
    cue = directory/'disc.cue'
    cue.write_text('FILE "data.iso" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
                   'FILE "audio.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n', encoding='ascii')
    data_sectors = data['size']//BLOCK
    return {'schema_version': 1, 'format': 'cue-multifile', 'cue': str(cue.resolve()),
            'data': data, 'audio': {'path': str(audio.resolve()), 'sha256': sha(audio),
            'size': audio.stat().st_size, 'rate': 44100, 'channels': 2, 'sample_format': 'S16LE',
            'provenance': 'CiukiOS self-authored integer square wave'},
            'tracks': [{'number': 1, 'type': 'MODE1/2048', 'index01_lba': 0, 'sectors': data_sectors},
                       {'number': 2, 'type': 'AUDIO', 'index00_lba': data_sectors,
                        'index01_lba': data_sectors+150, 'sectors': sectors}],
            'leadout_lba': data_sectors+150+sectors}
