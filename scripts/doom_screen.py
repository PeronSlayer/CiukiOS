"""Identify Doom menus and its static statusbar from actual WAD patches."""
import struct
import numpy as np
from PIL import Image


def wad_lumps(wad):
    count, directory = struct.unpack_from('<II', wad, 4)
    lumps = {}
    for i in range(count):
        offset, size, name = struct.unpack_from('<II8s', wad, directory+i*16)
        lumps[name.rstrip(b'\0')] = wad[offset:offset+size]
    return lumps


def decode_patch(raw):
    width, height, left, top = struct.unpack_from('<hhhh', raw)
    indices = np.zeros((height, width), dtype=int)
    mask = np.zeros((height, width), dtype=bool)
    for x in range(width):
        offset, = struct.unpack_from('<I', raw, 8+x*4)
        while raw[offset] != 255:
            y, length = raw[offset:offset+2]
            indices[y:y+length, x] = list(raw[offset+3:offset+3+length])
            mask[y:y+length, x] = True
            offset += length+4
    return indices, mask, left, top


def wad_palettes(lumps):
    # Pickup/damage flashes affect menus over an attract-mode demo too.
    palettes = np.frombuffer(lumps[b'PLAYPAL'], dtype=np.uint8).reshape(-1, 256, 3).astype(int)
    return (palettes >> 2)*255//63


def menu_skulls(wad):
    lumps = wad_lumps(wad)
    palettes = wad_palettes(lumps)
    result = []
    for name in (b'M_SKULL1', b'M_SKULL2'):
        indices, mask, left, top = decode_patch(lumps[name])
        result.extend((palette[indices], mask, left, top) for palette in palettes)
    return result


def statusbar_patterns(wad):
    """Mask dynamic widgets at id Software's st_stuff/st_lib coordinates."""
    lumps = wad_lumps(wad)
    indices, mask, left, top = decode_patch(lumps[b'STBAR'])
    assert indices.shape == (32, 320) and left == top == 0, 'Unsupported STBAR layout'

    def exclude(x, y, width, height):
        y -= 168
        mask[max(0, y):min(32, y+height), max(0, x):min(320, x+width)] = False

    def patch_rect(name, x, y):
        width, height, xoffset, yoffset = struct.unpack_from('<hhhh', lumps[name])
        exclude(x-xoffset, y-yoffset, width, height)

    def numbers(prefix, x, y):
        width, height = struct.unpack_from('<hh', lumps[prefix + b'0'])
        exclude(x-3*width, y, 3*width, height)  # Widget's background restore.
        for digit in range(10):
            for place in range(1, 4):
                patch_rect(prefix + str(digit).encode(), x-place*width, y)

    # Weapons/arms background and animated face overwrite this panel.
    exclude(104, 168, 75, 32)
    for x in (44, 90, 221):
        numbers(b'STTNUM', x, 171)
    for x in (90, 221):
        patch_rect(b'STTPRCNT', x, 171)
    for x in (288, 314):
        for y in (173, 179, 185, 191):
            numbers(b'STYSNUM', x, y)
    for y in (171, 181, 191):
        for number in range(6):
            patch_rect(b'STKEYS' + str(number).encode(), 239, y)
    assert mask.sum() >= 2000, 'Too few independent static STBAR pixels'
    return [(palette[indices], mask.copy()) for palette in wad_palettes(lumps)]


def statusbar_score(image, patterns):
    frame = np.asarray(image.resize((320, 200), Image.Resampling.NEAREST)).astype(int)[168:]
    scores = [float((np.max(np.abs(frame[mask]-pixels[mask]), axis=1) <= 8).mean())
              for pixels, mask in patterns]
    palette = int(np.argmax(scores))
    return dict(matched=scores[palette] >= .985, score=round(scores[palette], 5),
                palette=palette, compared_pixels=int(patterns[palette][1].sum()))


def gameplay_screen(image, patterns, sprites):
    """An attract-mode level behind a menu must not pass gameplay readiness."""
    menus = {name: selected_row(image, sprites, origin, rows) for name, origin, rows in (
        ('main', (97, 64), 6), ('episode', (48, 63), 4), ('skill', (48, 63), 5))}
    status = statusbar_score(image, patterns)
    return dict(matched=status['matched'] and all(row is None for row in menus.values()),
                statusbar=status, menus=menus)


def selected_row(image, sprites, origin=(97, 64), rows=6):
    frame = np.array(image.resize((320, 200), Image.Resampling.NEAREST)).astype(int)
    matches = []
    for row in range(rows):
        for pixels, mask, left, top in sprites:
            x = origin[0]-32-left
            y = origin[1]-5+row*16-top
            height, width = mask.shape
            actual = frame[y:y+height, x:x+width]
            if actual.shape != pixels.shape:
                continue
            # Doom's gamma-0 table and the VGA DAC can shift a channel by
            # two six-bit steps. Spatial sprite matching still uses >98%
            # of all nontransparent pixels, for both animation frames.
            score = float((np.max(np.abs(actual[mask]-pixels[mask]), axis=1) <= 8).mean())
            if score > .98:
                matches.append(row)
    found = set(matches)
    return found.pop() if len(found) == 1 else None
