"""Identify Doom's selected menu row from its actual WAD skull sprites."""
import struct
import numpy as np
from PIL import Image


def menu_skulls(wad):
    count, directory = struct.unpack_from('<II', wad, 4)
    lumps = {}
    for i in range(count):
        offset, size, name = struct.unpack_from('<II8s', wad, directory+i*16)
        lumps[name.rstrip(b'\0')] = wad[offset:offset+size]
    # Pickup/damage flashes affect menus over an attract-mode demo too.
    palettes = np.frombuffer(lumps[b'PLAYPAL'], dtype=np.uint8).reshape(-1, 256, 3).astype(int)
    palettes = (palettes >> 2)*255//63
    result = []
    for name in (b'M_SKULL1', b'M_SKULL2'):
        raw = lumps[name]
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
        result.extend((palette[indices], mask, left, top) for palette in palettes)
    return result


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
