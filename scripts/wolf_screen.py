"""Recognize original Wolf3D screens from the installed WL6 picture assets.

Formats and coordinates follow id Software's WOLFSRC ID_CA.C, ID_VH.C,
GFXV_WL6.H, WL_MENU.C and WL_AGENT.C. No commercial asset is embedded.
"""
import struct
import hashlib

import numpy as np


class WolfSignon:
    """Locate linked introscn by MZ relocation references and known digests.

    Hashes identify the installed original v1.4 image and id's SIGNON.OBJ
    image. Pixel bytes stay in the installed executable, outside the repo.
    """
    HASHES = frozenset((
        '999d6f405bd45a2575aff5c83cfc9da183bbf28d811859a892ca5abc983ed5c7',
        '4fd711cdde7929645a46dd572ab61feacc8d3b79844250835b0ce87312a2c999',
    ))

    def __init__(self, executable):
        assert executable[:2] == b'MZ' and len(executable) >= 28, 'Wolf signon requires original MZ'
        count = struct.unpack_from('<H', executable, 6)[0]
        header_bytes = struct.unpack_from('<H', executable, 8)[0] * 16
        relocations = struct.unpack_from('<H', executable, 24)[0]
        assert 28 <= header_bytes <= len(executable)
        assert relocations >= 28 and relocations + count * 4 <= header_bytes
        segments = set()
        for offset, segment in struct.iter_unpack('<HH', executable[relocations:relocations + count * 4]):
            location = header_bytes + segment * 16 + offset
            assert location + 2 <= len(executable), 'Invalid Wolf MZ relocation'
            segments.add(struct.unpack_from('<H', executable, location)[0])
        candidates = []
        for segment in sorted(segments):
            offset = header_bytes + segment * 16
            if offset + 64000 > len(executable):
                continue
            raw = executable[offset:offset + 64000]
            digest = hashlib.sha256(raw).hexdigest()
            if digest in self.HASHES:
                candidates.append((segment, offset, digest, raw))
        assert len(candidates) == 1, f'Wolf linked signon bitmap unsupported/ambiguous: {len(candidates)}'
        segment, offset, digest, raw = candidates[0]
        self.picture = np.frombuffer(raw, np.uint8).reshape(200, 320)
        self.evidence = dict(file_offset=offset, module_segment=segment,
                             image_sha256=digest, referenced_segments=len(segments))


class WolfPalette:
    """Read the linked GAMEPAL.OBJ palette without copying it into the repo."""
    SHA256 = '9c6d75cf32e883cc936374b718b0d628e9cf449cecaa32c1415f2877aefe1670'

    def __init__(self, executable):
        # The first four standard EGA colours identify candidate DAC blocks.
        prefix = bytes((0, 0, 0, 0, 0, 42, 0, 42, 0, 0, 42, 42))
        candidates = []
        offset = executable.find(prefix)
        while offset >= 0:
            raw = executable[offset:offset + 768]
            if len(raw) == 768 and hashlib.sha256(raw).hexdigest() == self.SHA256:
                candidates.append((offset, raw))
            offset = executable.find(prefix, offset + 1)
        assert len(candidates) == 1, f'Wolf linked palette unsupported/ambiguous: {len(candidates)}'
        offset, raw = candidates[0]
        dac = np.frombuffer(raw, np.uint8).reshape(256, 3).astype(np.int16)
        assert dac.max() <= 63
        self.rgb = ((dac << 2) | (dac >> 4)).astype(np.uint8)
        self.evidence = dict(file_offset=offset, sha256=self.SHA256)


class WolfQuitMessages:
    """Read the unchanged engine's nine80-byte endStrings entries."""
    SHA256 = '92e4a9ecd5acb041a4efb63f637b2343641daf047e3024c88f98b81626701b78'

    def __init__(self, executable):
        offset = executable.find(b'Dost thou wish to\n')
        assert offset >= 0, 'Original Wolf endStrings were not found'
        raw = executable[offset:offset + 9 * 80]
        assert hashlib.sha256(raw).hexdigest() == self.SHA256, 'Unsupported Wolf endStrings block'
        self.messages = tuple(record.split(b'\0', 1)[0].decode('ascii')
                              for record in (raw[i:i+80] for i in range(0, len(raw), 80)))
        assert len(self.messages) == 9 and all(self.messages)
        self.evidence = dict(file_offset=offset, sha256=self.SHA256, messages=len(self.messages))


class WolfPictures:
    def __init__(self, header, dictionary, graph):
        assert len(header) % 3 == 0 and len(dictionary) >= 255 * 4
        self.offsets = [int.from_bytes(header[i:i+3], 'little') for i in range(0, len(header), 3)]
        self.nodes = [struct.unpack_from('<HH', dictionary, i * 4) for i in range(255)]
        self.graph = graph
        self.sizes = self.chunk(0)
        self.cache = {}
        assert self.picture(86).shape == (40, 320), 'Unsupported WL6 statusbar layout'
        assert self.picture(87).shape == (200, 320), 'Unsupported WL6 title layout'

    def chunk(self, number):
        start = self.offsets[number]
        assert start != 0xffffff and start + 4 <= len(self.graph), number
        end = next((p for p in self.offsets[number+1:] if p != 0xffffff), len(self.graph))
        assert start + 4 <= end <= len(self.graph), number
        length = struct.unpack_from('<I', self.graph, start)[0]
        assert 0 < length <= 64000, (number, length)
        source = self.graph[start+4:end]
        result = bytearray()
        node = 254
        for byte in source:
            for bit in range(8):
                value = self.nodes[node][(byte >> bit) & 1]
                if value < 256:
                    result.append(value)
                    node = 254
                    if len(result) == length:
                        return bytes(result)
                else:
                    node = value - 256
                    assert node < 255, 'Invalid Huffman node'
        raise ValueError(f'Truncated Wolf picture chunk{number}')

    def picture(self, number):
        if number not in self.cache:
            width, height = struct.unpack_from('<HH', self.sizes, (number - 3) * 4)
            assert width and width % 4 == 0 and height and width * height <= 64000
            raw = self.chunk(number)
            assert len(raw) == width * height, (number, width, height, len(raw))
            # Four byte planes contain every fourth horizontal pixel.
            planes = np.frombuffer(raw, np.uint8).reshape(4, height, width // 4)
            self.cache[number] = planes.transpose(1, 2, 0).reshape(height, width)
        return self.cache[number]

    def text(self, value, number=0):
        """Render installed STARTFONT glyph coverage as ID_VH.C does."""
        font = self.chunk(1 + number)
        assert len(font) >= 770
        height = struct.unpack_from('<H', font)[0]
        assert 0 < height <= 32, ('font height', height)
        locations = struct.unpack_from('<256H', font, 2)
        widths = font[514:770]
        glyphs = []
        for char in value.encode('ascii'):
            width, start = widths[char], locations[char]
            assert width <= 32 and start + width * height <= len(font)
            glyphs.append(np.frombuffer(font[start:start + width * height], np.uint8)
                          .reshape(height, width) != 0)
        return np.concatenate(glyphs, axis=1)


def pattern_score(frame, picture, x, y, mask=None, *, contrast_min=80):
    """Compare indexed spatial colour classes without assuming a DAC palette.

    Each expected index must map to a consistent observed RGB colour across
    its pixels. Distinct classes and contrast reject blank/fading frames.
    The dominant background is excluded so it cannot conceal missing detail.
    """
    height, width = picture.shape
    actual = np.asarray(frame)[y:y+height, x:x+width].astype(np.int16)
    if actual.shape != (height, width, 3):
        return dict(matched=False, score=0.0, pixels=0, distinct_colours=0)
    chosen = np.ones(picture.shape, bool) if mask is None else mask.copy()
    values, counts = np.unique(picture[chosen], return_counts=True)
    if not len(values):
        return dict(matched=False, score=0.0, pixels=0, distinct_colours=0)
    chosen &= picture != values[np.argmax(counts)]
    errors, colours = [], []
    for index in np.unique(picture[chosen]):
        sample = actual[chosen & (picture == index)]
        if len(sample) < 3:
            continue
        colour = np.median(sample, axis=0)
        colours.append(colour)
        errors.append(np.max(np.abs(sample - colour), axis=1))
    if not errors:
        return dict(matched=False, score=0.0, pixels=0, distinct_colours=0)
    errors = np.concatenate(errors)
    colours = np.asarray(colours)
    distinct = len(np.unique(colours.astype(int) // 8, axis=0))
    contrast = int(np.ptp(colours, axis=0).max())
    score = float(np.mean(errors <= 8))
    return dict(matched=score >= .985 and distinct >= 4 and contrast >= contrast_min,
                score=round(score, 5), pixels=len(errors), distinct_colours=distinct,
                contrast=contrast)


def complete_picture_score(frame, picture, x, y, palette, *, contrast_min=80, mask=None):
    """Require spatial shape and the installed full palette before input.

    Class consistency alone is also true during a fade. Two RGB units allow
    DAC expansion rounding, while the whole asset still needs 99.5% agreement.
    """
    pattern = pattern_score(frame, picture, x, y, mask, contrast_min=contrast_min)
    height, width = picture.shape
    actual = np.asarray(frame)[y:y+height, x:x+width].astype(np.int16)
    if actual.shape != (height, width, 3):
        return dict(pattern, matched=False, full_palette_score=0.0)
    expected = palette.rgb[picture].astype(np.int16)
    selected = np.ones(picture.shape, bool) if mask is None else mask
    colour_score = float(np.mean(np.max(np.abs(actual[selected] - expected[selected]), axis=1) <= 2))
    return dict(pattern, matched=pattern['matched'] and colour_score >= .995,
                full_palette_score=round(colour_score, 5), palette_pixels=int(selected.sum()))


def glyph_score(frame, coverage, x, y):
    """Require both exact installed-font foreground and background classes."""
    height, width = coverage.shape
    actual = np.asarray(frame)[y:y+height, x:x+width].astype(np.int16)
    if actual.shape != (height, width, 3) or not coverage.any() or coverage.all():
        return dict(matched=False, score=0.0, pixels=0)
    foreground = np.median(actual[coverage], axis=0)
    background = np.median(actual[~coverage], axis=0)
    errors = np.max(np.abs(actual - np.where(coverage[..., None], foreground, background)), axis=2)
    score = float(np.mean(errors <= 8))
    contrast = int(np.max(np.abs(foreground - background)))
    return dict(matched=score >= .995 and contrast >= 80,
                score=round(score, 5), pixels=height * width,
                foreground_pixels=int(coverage.sum()), contrast=contrast)


def signon_score(frame, pictures, signon):
    mask = np.ones((200, 320), bool)
    # Original WL_MENU.C IntroScreen changes these memory bars/check boxes.
    for x in (49, 89, 129):
        for row in range(10):
            y = 163 - 8 * row
            mask[y:y+5, x:x+6] = False
    for y in (82, 105, 128, 151, 174):
        mask[y:y+2, 164:176] = False
    # FinishSignon clears this strip and prints Press a key or Working...
    # Match the waiting prompt separately, including its blank glyph pixels.
    mask[189:200, :300] = False
    bitmap = pattern_score(frame, signon.picture, 0, 0, mask)
    prompt = pictures.text('Press a key')
    text = glyph_score(frame, prompt, (320 - prompt.shape[1]) // 2, 190)
    return dict(matched=bitmap['matched'] and text['matched'],
                score=min(bitmap['score'], text['score']), screen='signon-awaiting-key',
                bitmap=bitmap, prompt=text)


def cursor_score(frame, pictures, origin, row):
    x, y = origin
    return max((pattern_score(frame, pictures.picture(number), x & ~7, y - 2 + row * 13)
                for number in (11, 12)), key=lambda result: (result['matched'], result['score']))


def episode_score(frame, pictures):
    # The installed C_EPISODE1PIC's complete menu palette spans65 channels,
    # unlike brighter title/cursor assets. Keep this exception asset-specific.
    thumbnail = pattern_score(frame, pictures.picture(30), 40, 23, contrast_min=64)
    cursor = cursor_score(frame, pictures, (10, 23), 0)
    return dict(thumbnail, matched=thumbnail['matched'] and cursor['matched'],
                cursor=cursor, picture=30, required_thumbnail_contrast=64)


def hud_score(frame, pictures, palette=None):
    picture = pictures.picture(86)
    mask = np.ones(picture.shape, bool)
    # WL_AGENT.C StatusDrawPic uses x*8,y relative to y160. Exclude only
    # the changing face/weapon/keys and the right-justified numeric fields.
    for x, y, width, height in (
        (136, 4, 24, 32), (240, 4, 16, 32), (256, 8, 64, 32),
        (16, 16, 16, 16), (48, 16, 48, 16), (112, 16, 8, 16),
        (168, 16, 24, 16), (216, 16, 16, 16),
    ):
        mask[y:y+height, x:x+width] = False
    if palette is not None:
        return complete_picture_score(frame, picture, 0, 160, palette, mask=mask)
    return pattern_score(frame, picture, 0, 160, mask)


def gameplay_score(frame, pictures, palette):
    hud = hud_score(frame, pictures, palette)
    loading = pattern_score(frame, pictures.picture(134), 48, 56)
    return dict(hud, matched=hud['matched'] and not loading['matched'],
                loading_picture=loading, screen='settled-gameplay')


def quit_confirmation_score(frame, pictures, messages):
    """Require all installed font1 lines at Message's exact F10 coordinates.

    WL_MENU.C uses WindowH160, left-aligns every line and reserves ten
    extra pixels after the final line for Confirm's blinking cursor. That
    cursor lies outside every checked glyph rectangle.
    """
    candidates = []
    for number, message in enumerate(messages.messages):
        lines = [pictures.text(line, 1) for line in message.split('\n')]
        height = sum(line.shape[0] for line in lines)
        width = max([line.shape[1] for line in lines[:-1]] + [lines[-1].shape[1] + 10])
        x, y = 160 - width // 2, 80 - height // 2
        scores = []
        for line in lines:
            scores.append(glyph_score(frame, line, x, y))
            y += line.shape[0]
        candidates.append(dict(matched=all(score['matched'] for score in scores),
                               score=min(score['score'] for score in scores),
                               pixels=sum(score['pixels'] for score in scores),
                               message=number, lines=scores, screen='F10-quit-confirmation'))
    return max(candidates, key=lambda result: (result['matched'], result['score']))
