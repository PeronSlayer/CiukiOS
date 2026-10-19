# LZ32 compression sources

`mspack/` contains the unmodified KWAJ/SZDD decompression subset of
[libmspack](https://github.com/kyz/libmspack), pinned to commit
`55d501976171397ccd5d5a7a1ca7da065b1d9a06` (retrieved 2026-10-19).
Copyright Stuart Caie and the authors credited in each file, LGPL-2.1-only;
see `mspack/COPYING.LIB`. The pin and per-file SHA-256 are in
`mspack/UPSTREAM.json`. No compressor, default file system, or unrelated archive
decoder is included. `compat/` and the surrounding adapters are CiukiOS code.

The custom `mspack_system` supplies bounded memory buffers and the PE process
heap. It supports KWAJ methods 0–4 (copy, XOR, LZSS, LZH, MSZIP) and SZDD,
checks declared expanded lengths and rejects truncated output. Allocation
failure is an LZ error. Input and expanded output are individually limited to
64 MiB; expansion is buffered, rather than a constant-memory stream. This is
an explicit resource limit of the current facade. The original SDK functions
remain callable through kernel32 imports when f5-06 is integrated.

Source and license must accompany the distributed PE facade. The module is
independently rebuildable with `scripts/win32/facades.py`; it is LGPL code,
separate from proprietary installer/game payloads.
