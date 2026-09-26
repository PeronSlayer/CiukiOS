#!/usr/bin/env python3
"""Synthesize CiukiOS's original short bell motif; no sampled system sounds."""
import math
from pathlib import Path
import struct
import wave


def generate(output):
    # 15 DMA buffers, each 4096 stereo frames, exactly 1.28 seconds at 48 kHz.
    samples = bytearray()
    notes = ((0.00, 293.665, .21), (0.03, 587.330, .40),
             (0.16, 880.000, .34), (0.30, 987.767, .30),
             (0.45, 1479.978, .25))
    for n in range(61440):
        t = n / 48000
        value = 0.0
        for start, frequency, gain in notes:
            u = t - start
            if u >= 0:
                envelope = (1 - math.exp(-u * 180)) * math.exp(-u * 4.8)
                value += gain * envelope * (
                    math.sin(math.tau * frequency * u)
                    + .16 * math.sin(math.tau * frequency * 2.01 * u))
        fade = min(1.0, (1.28 - t) / .16)
        pcm = round(max(-1, min(1, value * fade)) * 26000)
        samples.extend(struct.pack('<hh', pcm, pcm))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'BOOT.PCM').write_bytes(samples)
    # The same melody for an ISA DSP: unsigned mono, exactly 8 kHz.
    (output / 'BOOTSB.PCM').write_bytes(bytes(
        (struct.unpack_from('<h', samples, frame * 4)[0] >> 8) + 128
        for frame in range(0, 61440, 6)))
    with wave.open(str(output / 'ciukios-startup.wav'), 'wb') as wav:
        wav.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        wav.writeframes(samples)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
