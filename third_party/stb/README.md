# stb_image

`stb_image.h` is the unmodified stb_image v2.30 single-file image loader by
Sean Barrett and contributors, from <https://github.com/nothings/stb>, file
`stb_image.h` at commit `f58f558c120e9b32c217290b80bad1a0729fbb2c`
(<https://raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image.h>).
SHA-256 of the file: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.

Licence: the upstream file offers two alternatives, the MIT License or the
Public Domain dedication (Unlicense); the notice is retained at the end of
`stb_image.h` and copied to `LICENSE`. CiukiOS uses it under either.

Use in CiukiOS: the Image Viewer (`apps/accessories/viewer_stb.c`) compiles it
with `STBI_ONLY_PNG`, `STBI_ONLY_JPEG`, `STBI_ONLY_BMP`, `STBI_ONLY_GIF`,
`STBI_NO_STDIO`, `STBI_NO_HDR`, `STBI_NO_LINEAR`, `STBI_NO_SIMD` (the SDK
targets i686 with x87 and no SSE/MMX) and `STBI_NO_THREAD_LOCALS` (the SDK
excludes compiler TLS). The image is installed as
`/system/licenses/stb_image.txt`.
