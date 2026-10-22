# minimp3

Unmodified `minimp3.h` from [lieff/minimp3](https://github.com/lieff/minimp3)
at commit `ea99364f61c14656440e8d77e9c233ccf3124633`.
Source: https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3.h

License: CC0 1.0 Universal, full upstream text in `LICENSE`; installed as
`/system/licenses/minimp3.txt`. Music Player uses `MINIMP3_NO_SIMD` and
`MINIMP3_ONLY_MP3` for the i686 baseline and, since f4-37,
`MINIMP3_FLOAT_OUTPUT`: `apps/music/decoder.c` drives the header's layer III
functions in its own stage-timed frame loop and converts to int16 itself
(`docs/validation/f4-37-music-decoder-cost.md`). The header is unchanged.

SHA-256:

- minimp3.h: `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad`
- LICENSE: `36ffd9dc085d529a7e60e1276d73ae5a030b020313e6c5408593a6ae2af39673`
