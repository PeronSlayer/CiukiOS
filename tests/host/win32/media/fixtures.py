"""Project-authored media fixtures for f5-12B (no commercial or extracted media).

WAV: PCM and IMA/MS ADPCM (ADPCM blocks from the f5-14C authored generator,
decoded independently by its scalar reference). MIDI: Standard MIDI Files
format 0/1 with a tempo map, running status, sysex and an RMID wrapper, plus
an independent tempo-map oracle. AVI: interleaved MS Video 1 (8-bit) video
and PCM or IMA ADPCM audio with independently computed frame pixels.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import struct
from tests.host.win32.codecs import fixtures as codec


def chunk(kind, payload):
    return codec.chunk(kind, payload)


def avi_index(data, absolute=False):
    """Add an authored old-style index to an unindexed, flat movi fixture."""
    origin = data.index(b'movi')
    end = origin + struct.unpack_from('<I', data, origin - 4)[0]
    entries = bytearray()
    at = origin + 4
    while at < end:
        size = struct.unpack_from('<I', data, at + 4)[0]
        entries += struct.pack('<4sIII', data[at:at+4], 0x10,
                               at if absolute else at - origin, size)
        at += 8 + size + (size & 1)
    return chunk(b'RIFF', data[8:] + chunk(b'idx1', entries))


def wav_pcm(rate=22050, channels=1, bits=16, frames=11025):
    align = channels * bits // 8
    data = bytearray()
    for i in range(frames):
        for c in range(channels):
            if bits == 16:
                data += struct.pack('<h', ((i * 37 + c * 1111) % 4001 - 2000) * 7)
            else:
                data.append((i * 5 + c * 77) & 255)
    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * align, align, bits)
    return chunk(b'RIFF', b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'LIST', b'INFOjunk') + chunk(b'data', bytes(data)))


def wav_adpcm(tag, channels, rate, alignment, blocks):
    wave, block, ref = codec.audio(tag, channels, rate, alignment)
    data = b''
    pcm = b''
    for n in range(blocks):
        b = bytearray(block)
        header = (4 if tag == 17 else 7) * channels
        for i in range(header, len(b)):
            b[i] = (b[i] + n * 29) & 255
        data += bytes(b)
        pcm += codec.reference(tag, channels, bytes(b))
    samples = struct.unpack_from('<H', wave, 18)[0]
    fact = struct.pack('<I', samples * blocks)
    riff = chunk(b'RIFF', b'WAVE' + chunk(b'fmt ', wave) + chunk(b'fact', fact) + chunk(b'data', data))
    return riff, pcm


def vlq(n):
    out = [n & 127]
    n >>= 7
    while n:
        out.append(128 | (n & 127))
        n >>= 7
    return bytes(reversed(out))


def track(events):
    """events: (delta, bytes) with running status already applied by caller."""
    body = b''.join(vlq(d) + e for d, e in events)
    return b'MTrk' + struct.pack('>I', len(body)) + body


def midi_song():
    """Format 1, 96 PPQN: tempo 120 bpm, at tick 192 tempo 240 bpm."""
    conductor = track([(0, b'\xff\x51\x03' + (500000).to_bytes(3, 'big')),
                       (192, b'\xff\x51\x03' + (250000).to_bytes(3, 'big')),
                       (192, b'\xff\x2f\x00')])
    notes = track([(0, b'\xf0\x05\x7e\x7f\x09\x01\xf7'),  # GM system on
                   (0, b'\xc0\x05'), (0, b'\xb0\x07\x64'), (0, b'\x0a\x30'),  # running status
                   (0, b'\x90\x3c\x40'), (96, b'\x3c\x00'),
                   (0, b'\xe0\x00\x50'),
                   (96, b'\x90\x40\x50'), (96, b'\x80\x40\x00'),
                   (0, b'\xb0\x40\x7f'),
                   (48, b'\x90\x43\x60'), (48, b'\x43\x00'),
                   (0, b'\xff\x2f\x00')])
    header = b'MThd' + struct.pack('>IHHH', 6, 1, 2, 96)
    return header + conductor + notes


def midi_smpte():
    """Format 0, SMPTE 25 fps, 40 ticks per frame (1 tick = 1 ms)."""
    body = track([(0, b'\xc1\x10'), (0, b'\x91\x30\x40'), (500, b'\x81\x30\x00'), (500, b'\xff\x2f\x00')])
    division = ((256 - 25) << 8) | 40
    return b'MThd' + struct.pack('>IHHH', 6, 0, 1, division) + body


def rmid(smf):
    return chunk(b'RIFF', b'RMID' + chunk(b'data', smf))


def tempo_events_us(smf_events, ppqn, tempos):
    """Independent oracle: absolute microseconds of ticks under a tempo map."""
    def us(tick):
        total = 0
        last_tick, last_tempo = 0, 500000
        for t, value in tempos:
            if t >= tick:
                break
            total += (t - last_tick) * last_tempo // ppqn
            last_tick, last_tempo = t, value
        return total + (tick - last_tick) * last_tempo // ppqn
    return [us(t) for t in smf_events]


def song_oracle():
    """Note/controller events of midi_song in merge order with absolute us."""
    ticks = [0, 0, 0, 0, 0, 96, 96, 192, 288, 288, 336, 384]
    tempos = [(0, 500000), (192, 250000)]
    return ticks, tempo_events_us(ticks, 96, tempos), tempo_events_us([384], 96, tempos)[0]


def video_frame(index, width=32, height=16):
    """Bottom-up 8-bit DIB pixels and the MS Video 1 key frame encoding them.

    Each 4x4 block is a fill code (colour byte, 0x80) in decode order: block
    rows from the bottom, left to right (Microsoft Video 1 format notes).
    """
    bw, bh = width // 4, height // 4
    codes = b''
    pixels = bytearray(width * height)
    for by in range(bh):
        for bx in range(bw):
            colour = (index * 40 + bx * 9 + by * 31) % 251 + 2
            codes += bytes((colour, 0x80))
            for y in range(4):
                for x in range(4):
                    pixels[(by * 4 + y) * width + bx * 4 + x] = colour
    return codes + b'\x00\x00', bytes(pixels)


def avi_clip(frames=6, width=32, height=16, audio='pcm'):
    """Interleaved AVI: 'vids' msvc 8-bit at 15 fps and an 'auds' stream."""
    palette = b''.join(bytes((i, i, i, 0)) for i in range(256))
    vfmt = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 8, int.from_bytes(b'CRAM', 'little'), 0, 0, 0, 256, 0) + palette
    vstrh = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'msvc', 0, 0, 0, 0, 1, 15, 0, frames, 0, 0, 0, 0, 0, width, height)
    rate = 22050  # the mixer's own rate: the sound track passes unchanged
    if audio == 'pcm':
        afmt = struct.pack('<HHIIHHH', 1, 1, rate, rate * 2, 2, 16, 0)
        per_frame = rate // 15
        chunks = []
        pcm = b''
        for f in range(frames):
            data = b''.join(struct.pack('<h', ((f * 911 + i * 37) % 6001) - 3000) for i in range(per_frame))
            chunks.append(data)
            pcm += data
        ascale, arate, align, alength = 1, rate, 2, per_frame * frames
    else:
        wave, block, _ = codec.audio(17, 1, rate, 512)
        afmt = wave
        chunks = []
        pcm = b''
        for f in range(frames):
            b = bytearray(block)
            for i in range(4, len(b)):
                b[i] = (b[i] + f * 13) & 255
            chunks.append(bytes(b))
            pcm += codec.reference(17, 1, bytes(b))
        samples = struct.unpack_from('<H', wave, 18)[0]
        ascale, arate, align, alength = samples, rate, 512, frames
    astrh = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'auds', b'\0\0\0\0', 0, 0, 0, 0, ascale, arate, 0, alength, 0, 0, align, 0, 0, 0, 0)
    avih = struct.pack('<IIIIIIIIII4I', 66667, 0, 0, 0x10, frames, 0, 2, 0, width, height, 0, 0, 0, 0)
    hdrl = b'hdrl' + chunk(b'avih', avih) + chunk(b'LIST', b'strl' + chunk(b'strh', vstrh) + chunk(b'strf', vfmt)) + \
        chunk(b'LIST', b'strl' + chunk(b'strh', astrh) + chunk(b'strf', afmt))
    movi = b'movi'
    images = []
    for f in range(frames):
        codes, pixels = video_frame(f, width, height)
        images.append(pixels)
        movi += chunk(b'01wb', chunks[f]) + chunk(b'00dc', codes)
    riff = chunk(b'RIFF', b'AVI ' + chunk(b'LIST', hdrl) + chunk(b'LIST', movi))
    return avi_index(riff), images, pcm


def write_all(folder):
    from pathlib import Path
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'tone.wav').write_bytes(wav_pcm())
    (folder / 'tone8.wav').write_bytes(wav_pcm(11025, 2, 8, 3300))
    ima, ima_pcm = wav_adpcm(17, 1, 22050, 256, 12)
    (folder / 'ima.wav').write_bytes(ima)
    (folder / 'ima.pcm').write_bytes(ima_pcm)
    ms, ms_pcm = wav_adpcm(2, 2, 22050, 512, 6)
    (folder / 'msadpcm.wav').write_bytes(ms)
    (folder / 'msadpcm.pcm').write_bytes(ms_pcm)
    (folder / 'song.mid').write_bytes(midi_song())
    # Two directory spellings of one identity, as with a FAT LFN/8.3 pair.
    # A separate byte-identical file prevents content/name based ownership.
    for source, long_name, short_name, distinct in (
            ('tone.wav', 'Long Tone.wav', 'LONGTO~1.WAV', 'Long Tone 2.wav'),
            ('song.mid', 'Long Song.mid', 'LONGSO~1.MID', 'Long Song 2.mid')):
        (folder / long_name).write_bytes((folder / source).read_bytes())
        alias = folder / short_name
        alias.unlink(missing_ok=True)
        alias.hardlink_to(folder / long_name)
        (folder / distinct).write_bytes((folder / source).read_bytes())
    (folder / 'song.rmi').write_bytes(rmid(midi_song()))
    (folder / 'smpte.mid').write_bytes(midi_smpte())
    (folder / 'broken.mid').write_bytes(midi_song()[:40])
    (folder / 'broken.wav').write_bytes(wav_pcm()[:30])
    for kind in ('pcm', 'ima'):
        riff, images, pcm = avi_clip(audio=kind)
        (folder / f'clip-{kind}.avi').write_bytes(riff)
        (folder / f'clip-{kind}.pcm').write_bytes(pcm)
        (folder / f'clip-{kind}.frames').write_bytes(b''.join(images))
    # Structurally valid RIFF: corruption is inside a later codec payload.
    for name, audio, target, which, frames in (
            ('bad-video', 'pcm', b'00dc', 1, 6),
            ('bad-audio-first', 'ima', b'01wb', 0, 6),
            ('bad-audio-late', 'ima', b'01wb', 30, 36)):
        riff, _, _ = avi_clip(frames=frames, audio=audio)
        data = bytearray(riff)
        movi = data.index(b'movi') + 4
        at, index = movi, 0
        while at + 8 <= len(data):
            kind, size = struct.unpack_from('<4sI', data, at)
            if kind == target:
                if index == which:
                    if target == b'00dc': data[at+8:at+8+size] = bytes(size)
                    else: data[at+10] = 255  # invalid IMA step index
                    break
                index += 1
            at += 8 + size + (size & 1)
        (folder / (name + '.avi')).write_bytes(data)
    return folder
