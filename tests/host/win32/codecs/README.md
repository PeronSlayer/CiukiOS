# P5 codec evidence

Run `nice -n 19 python3 -B -m unittest tests.host.win32.codecs.test_codecs -v`.
No original/commercial media is included. `fixtures.py` authors ADPCM blocks,
Cinepak codebooks and frames, and small RIFF AVI files. RLE and Video 1 test
vectors are authored directly in `test_codecs.py`. `goldens.json` pins frame
SHA-256 values calculated from explicit expected pixel arrays, independently
of the production decoder. The scalar Python ADPCM reference compares every
output sample, including truncated final blocks, against production ACM calls.

The native adapter links the production C directly and replaces only heap and
file operations. It is H0 evidence, not PE execution. Separate native DriverProc
builds exercise ACMDM/ICM messages; compile-time assertions check actual i386
layouts. The ASan/UBSan executable mutates authored seeds and random inputs for
24,000 iterations, including truncation, excess data and chunk-size corruption.
LeakSanitizer cannot use ptrace in this sandbox; ASan/UBSan remain enabled and
the adapter separately asserts that all heap allocations have been released.

`wine_cases.c` selects PCM format-suggestion and codec-enumeration assertions
from the pinned Wine 10.0 tests, attributed in the file. They execute as native
C and are also built as an actual i386 PE. The H1 execution test skips only on
SIGSYS from the Linux adapter bootstrap, recording the seccomp restriction.
This is not execution of Wine's complete test programs or of a guest DLL.
The complete upstream tests also require APIs outside the P5 decode subset.

`codecs-inventory.json` records every exported function's coverage, exact PE
imports, missing imports, hashes, source/notices pins, image bytes and the cold
store memory model. The host store test loads/binds/unloads actual PEs through
the production loader on H0 with calls observed; all P5 DLLs use /noentry and
need no DllMain side effects. Real i386 guest loading remains the lead's gate.

Supported decode output: ADPCM to signed PCM16 at the source channel count and
rate; RLE4/RLE8 to indexed 8-bit DIBs; Video 1 to indexed 8-bit or RGB555 DIBs;
Cinepak to BGR24. ADPCM retains its source rate; the separate ACM PCM
converter supports channel/bit-depth/rate conversion. No encoder or silent
codec substitution is provided.
The decoder retains skipped pixels and Cinepak books within a stream. Callers
must preserve their output DIB across delta frames and restart after malformed
input; errors may leave a partial frame or changed codebook. ACM supports
synchronous conversion; incomplete blocks wait for END, which decodes the
available complete samples/channel groups. Header truncations return errors.
BLOCKALIGN leaves incomplete blocks unconsumed, even with END.

AVI is a read-only, API-level RIFF AVI reader: four streams/file, eight open
files, 65,536 chunks/stream, 16 frame getters, 32 stream views; depth <= 8 and
one million RIFF chunks. OpenDML/AVIX, palette-change chunks, edit/write/COM
interfaces are unsupported. Video frames decode from the beginning after a
backwards seek to preserve delta
state. Optional idx1 entries are checked against physical movi chunks (including
relative and absolute offset conventions); their offsets never authorize new
reads. Fixed-size streams expose sample units and reads may cross interleaved
chunks or fill a smaller buffer with complete samples. Variable-size streams
retain one sample per chunk. W filenames are converted without
best-fit substitution to the core's ACP; unrepresentable paths fail cleanly.
The APIs currently require serialized calls (including GetFrame on one file).

`w32-codecs-qemu-t23` is authored in the suite with `integration_pending`;
no guest probe, QEMU run, hardware run or image evidence is claimed here.
