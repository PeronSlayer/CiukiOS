# Original decoder fixtures

These 0.1-second 440/660 Hz mathematical tones and their generator are
released under CC0-1.0 by the CiukiOS project. No commercial recording,
upstream test vector, or third-party music is included. `generate.py` uses
Python's wave module, then FFmpeg's libmp3lame/libvorbis encoders; generated
MP3 and Ogg files carry no copyright in an external recording.

Regenerate sequentially: `nice -n 19 python3 tests/fixtures/music/generate.py`.
Tests consume the checked-in bytes and do not require FFmpeg. The WAV matrix
covers both widths and channel counts, rates below and above the native
8000..48000 range. Host tests synthesize malformed RIFF cases separately.
`tone-mono.mp3` (f4-37) is the mono 96 kHz tone encoded like `tone.mp3`: it
exercises minimp3's mono synthesis path in the x87 decoder test.
