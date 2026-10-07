# Audio decoders

The media worker uses two upstream decoder projects, pinned here so that the
HDD/CD build does not depend on network access or a host library installation.

| Source | Pinned revision | Files | License |
| --- | --- | --- | --- |
| [mackron/dr_libs](https://github.com/mackron/dr_libs) | `dfe8377631000664666519fdb83da193fd8037f4` | `dr_libs/dr_wav.h`, `dr_mp3.h`, `dr_flac.h` | Unlicense or MIT-0; see `dr_libs/LICENSE` |
| [nothings/stb](https://github.com/nothings/stb) | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | `stb/stb_vorbis.c` | MIT or public domain; see `stb/LICENSE` |

The decoder boundary is a DOS/DPMI worker, not the 16-bit desktop module. WAV,
MP3 and FLAC use dr_libs' frame-oriented signed-16 readers; Ogg Vorbis uses
stb_vorbis' interleaved signed-16 reader. Each decoder is driven in bounded
chunks and can seek to a PCM frame. The app transports only chunks through the
shared worker mailbox and the AC'97 service; it does not allocate a whole-song
PCM buffer.

SHA-256 (vendored source as checked in):

```
03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493  dr_libs/dr_wav.h
997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f  dr_libs/dr_mp3.h
111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c  dr_libs/dr_flac.h
dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15  dr_libs/LICENSE
4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636  stb/stb_vorbis.c
8743310f039bf62b0e3818e798900719ef0d8f4e906637828e6a9485086528e0  stb/LICENSE
```
