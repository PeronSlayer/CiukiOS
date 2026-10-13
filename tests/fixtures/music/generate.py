#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Generate original short tones. No music recording or upstream test vector."""
from pathlib import Path
import math
import struct
import subprocess
import wave
ROOT = Path(__file__).resolve().parent
for bits in (8, 16):
    for channels in (1, 2):
        for rate in (6000, 96000):
            name = ROOT / f"pcm-{bits}-{channels}-{rate}.wav"
            with wave.open(str(name), 'wb') as out:
                out.setparams((channels, bits//8, rate, 0, 'NONE', 'not compressed'))
                data = bytearray()
                for i in range(rate//10):
                    for c in range(channels):
                        tone = math.sin(2*math.pi*(440 if c == 0 else 660)*i/rate)*0.4
                        data += bytes([round(128+tone*100)]) if bits == 8 else struct.pack('<h', round(tone*20000))
                out.writeframes(data)
for codec, ext in [('libmp3lame', 'mp3'), ('libvorbis', 'ogg')]:
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-threads', '1', '-i', str(ROOT/'pcm-16-2-96000.wav'),
                    '-ar', '44100', '-c:a', codec, str(ROOT/f'tone.{ext}')], check=True)
