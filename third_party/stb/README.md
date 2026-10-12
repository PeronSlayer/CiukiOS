# stb image codecs

`stb_image.h` is the unmodified upstream single-file image loader by Sean
Barrett, version 2.30, from <https://github.com/nothings/stb> at commit
`f58f558c120e9b32c217290b80bad1a0729fbb2c`
(`https://raw.githubusercontent.com/nothings/stb/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image.h`).
SHA-256 of the file: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`
(the same bytes are shipped by the Arch Linux `materialx` package, which was
used to cross-check the download).

The file is dual licensed, at the user's choice, under the MIT licence or as
Public Domain (Unlicense); the full text is in `LICENSE` and at the end of the
header. CiukiOS uses it under the MIT alternative and installs the notice as
`/system/licenses/stb_image.txt`.

Use in CiukiOS: `apps/desktop/image.c` compiles the PNG, JPEG (baseline and
progressive) and BMP decoders only (`STBI_ONLY_PNG`, `STBI_ONLY_JPEG`,
`STBI_ONLY_BMP`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR`,
`STBI_NO_SIMD`, `STBI_NO_THREAD_LOCALS`) for desktop wallpapers. The desktop
reads the file itself, checks the dimensions with `stbi_info_from_memory`
before decoding and refuses images above 4096 x 4096 or 16 Mpixel.

## stb_image_write

`stb_image_write.h` is Sean Barrett's upstream version 1.16, vendored from
`/usr/include/MaterialXRender/External/StbImage/stb_image_write.h` (the Arch
Linux MaterialX package). SHA-256:
`cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05`.
The unmodified file retains its complete MIT/Unlicense text. Upstream API:
<https://github.com/nothings/stb/blob/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image_write.h>.
The MIT alternative and copyright notice in `LICENSE` apply to both headers;
CiukPaint installs a separate copy as `/system/licenses/stb_image_write.txt`.

`apps/paint/stb.c` compiles PNG/BMP decoding and callback-based writing
(`STBI_WRITE_NO_STDIO`). `apps/paint/io.c` exports RGB PNG and 24-bit BMP
with the writer; original 8-bit BMP retains the legacy file layout. Both
input dimensions and the pixel count are checked before allocation.
