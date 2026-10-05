# CiukiOS mQuickJS integration

This directory contains a MicroQuickJS source snapshot fetched from
`bellard/mquickjs` on 2026-10-04. The repository does not record a matching
upstream commit identifier, so this date is provenance information, not a
claim that the files match current upstream `main`. The upstream `LICENSE` is
MIT and must remain with redistributed copies.

CiukiOS adds `worker_js_stdlib.c` as a generator wrapper and conditional
`CIUKIOS_WEB_WORKER` entries in `mqjs_stdlib.c`. Those entries expose a narrow
`document` object and `alert`; implementations are in `src/web/worker_js.c`.
The ordinary upstream standard-library generation remains unchanged when that
macro is absent. `mquickjs.c` also changes `JS_NewShortInt` from signed
left-shift to multiplication to avoid C undefined behavior for negative
integers; the encoder input remains within the engine's checked 31-bit range.

The generated standard-library table has a 512-byte atom-alignment requirement,
but the DJGPP COFF section cannot guarantee that alignment. The worker copies
the table to a 512-byte-aligned buffer. `mquickjs_build.c` emits an explicit
list of `JS_ROM_VALUE` word offsets; the worker rebases only those tagged
entries. It clones the generated C-function table and rebases its known ROM
name values as well, preserving immediate one-character names. A previous
whole-table scan for pointer-tagged words was unsafe at DOS load addresses: the
original table spanned `0x59f0..0x7f5c`, so packed atom bytes `Id\0\0`
(`0x6449`) looked like an in-range pointer and corrupted the `getElementById`
binding. The host regression uses that synthetic DOS range and confirms the
ASCII word remains unchanged while a declared relocation is adjusted.

The source adapter copies each bounded JavaScript source span into owned
storage and NUL-terminates it before `JS_Eval`. Upstream mQuickJS assumes
`input[input_len]` is NUL, while mailbox and file spans do not promise that
following byte is zero. Host ASan/UBSan checks passed on both 32-bit and 64-bit
host builds, including the low-address relocation case; this does not establish
VM runtime compatibility.

The worker build must generate `worker_js_stdlib.h` for a 32-bit target before
compiling `src/web/worker_js.c`. A host generator can be built from
`mquickjs_build.c`, `cutils.c`, and `worker_js_stdlib.c`. Run it once with
`-m32 -a` and redirect stdout to `mquickjs_atom.h`, then with `-m32` and
redirect stdout to `worker_js_stdlib.h`. Put both generated headers on the
target include path. Compile every target mQuickJS translation unit, including
`mquickjs.c`, with `src/web/worker_js_config.h` included before
`mquickjs_priv.h`; otherwise the custom class count changes the `JSContext`
layout. The target engine consists of `mquickjs.c`, `dtoa.c`, `libm.c`,
`cutils.c`, and the worker adapter. Do not compile or run the upstream REPL as
part of the worker.
