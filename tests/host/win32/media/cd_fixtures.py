"""Project-authored CUE/BIN fixtures; kernel never parses CUE text.

Single BINARY file, AUDIO/MODE1/2352 or a separate MODE1/2048 file. INDEX 00
is stored pregap, PREGAP is unstored silence; INDEX 01 defines TOC starts.
The test transport consumes the normalized .toc/.raw view of the same files.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
import shlex
import struct


def frames(text):
    if not re.fullmatch(r'\d{1,3}:\d{2}:\d{2}', text):
        raise ValueError('invalid MSF')
    m, s, f = map(int, text.split(':'))
    if s >= 60 or f >= 75:
        raise ValueError('invalid MSF')
    return (m * 60 + s) * 75 + f


def parse(path):
    """Return TOC descriptors and raw 2352-byte sectors for mock /dev/media."""
    path = Path(path)
    tracks, files, current = [], [], None
    for line in path.read_text().splitlines():
        p = shlex.split(line)
        if not p or p[0] == 'REM':
            continue
        if p[0] == 'FILE' and len(p) == 3 and p[2] == 'BINARY':
            name = Path(p[1])
            if name.is_absolute() or len(name.parts) != 1:
                raise ValueError('fixture file must be local')
            current = {'data': (path.parent / name).read_bytes(), 'tracks': []}
            files.append(current)
        elif p[0] == 'TRACK' and len(p) == 3 and current is not None:
            if p[2] not in ('AUDIO', 'MODE1/2352', 'MODE1/2048') or int(p[1]) != len(tracks) + 1 or len(tracks) >= 99:
                raise ValueError('track/type out of range')
            t = {'number': int(p[1]), 'type': p[2], 'index': {}, 'gap': 0}
            tracks.append(t); current['tracks'].append(t)
        elif p[0] == 'INDEX' and len(p) == 3 and tracks and p[1] in ('00', '01'):
            t = tracks[-1]
            if p[1] in t['index']:
                raise ValueError('duplicate index')
            t['index'][p[1]] = frames(p[2])
        elif p[0] == 'PREGAP' and len(p) == 2 and tracks and not tracks[-1]['gap']:
            tracks[-1]['gap'] = frames(p[1])
        else:
            raise ValueError('unsupported CUE record')
    if not tracks:
        raise ValueError('no tracks')
    raw, toc = bytearray(), []
    for f in files:
        ts = f['tracks']
        if not ts:
            raise ValueError('file without tracks')
        sizes = {2048 if t['type'] == 'MODE1/2048' else 2352 for t in ts}
        if len(sizes) != 1:
            raise ValueError('mixed sector sizes in one file')
        size = sizes.pop()
        if len(f['data']) % size or len(f['data']) > 16 * 1024 * 1024:
            raise ValueError('partial sector or oversized fixture')
        sectors = len(f['data']) // size
        for i, t in enumerate(ts):
            idx = t['index']
            if '01' not in idx:
                raise ValueError('missing INDEX 01')
            begin = idx.get('00', idx['01'])
            end = ts[i+1]['index'].get('00', ts[i+1]['index'].get('01', -1)) if i + 1 < len(ts) else sectors
            if begin < 0 or idx['01'] < begin or end <= idx['01'] or end > sectors or (i == 0 and begin != 0):
                raise ValueError('invalid index ordering/extent')
            raw.extend(bytes(t['gap'] * 2352))
            start = len(raw) // 2352 + idx['01'] - begin
            toc.append((t['number'], 0 if t['type'] == 'AUDIO' else 4, 1, 0, start))
            for sector in range(begin, end):
                block = f['data'][sector*size:(sector+1)*size]
                raw.extend(block if size == 2352 else block + bytes(2352-size))
    return toc, bytes(raw)


def normalize(cue, target):
    tracks, raw = parse(cue)
    record = struct.pack('<8I', 1, 824, 1, 1, 1, len(tracks), len(tracks), len(raw)//2352)
    record += b''.join(struct.pack('<4BI', *t) for t in tracks)
    record += bytes(8 * (99-len(tracks)))
    Path(str(target) + '.toc').write_bytes(record)
    Path(str(target) + '.raw').write_bytes(raw)
    return tracks, raw


def tone(sectors, offset=0):
    # Integer square wave with distinct left/right channels: exact PCM oracle.
    return b''.join(struct.pack('<hh', 6000 if (i+offset)//32 % 2 else -6000,
                              3000 if (i+offset)//48 % 2 else -3000)
                    for i in range(sectors*588))


def write_all(directory):
    directory = Path(directory); directory.mkdir(parents=True, exist_ok=True)
    (directory/'mixed.bin').write_bytes(bytes(8*2352) + tone(36) + tone(30, 21168))
    (directory/'mixed.cue').write_text('FILE "mixed.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 01 00:00:08\n TRACK 03 AUDIO\n INDEX 01 00:00:44\n')
    (directory/'audio.bin').write_bytes(tone(75))
    (directory/'audio.cue').write_text('FILE "audio.bin" BINARY\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 00 00:00:30\n INDEX 01 00:00:40\n')
    (directory/'pregap.cue').write_text('FILE "audio.bin" BINARY\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n PREGAP 00:00:10\n INDEX 01 00:00:30\n')
    (directory/'data.bin').write_bytes(bytes(12*2048))
    (directory/'data.cue').write_text('FILE "data.bin" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n')
    for name in ('mixed', 'audio', 'pregap', 'data'):
        normalize(directory/(name+'.cue'), directory/name)
