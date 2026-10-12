# Tiny C Compiler for CiukiOS

Upstream release 0.9.27, tag `release_0_9_27`, commit
`d348a9a51d32cece842b7885d27a411436d7887b`:
https://github.com/TinyCC/tinycc/tree/release_0_9_27

The unmodified compiler, runtime and compiler headers are in `upstream/`.
Examples, test suites and Windows-specific headers are omitted. `UPSTREAM.json`
pins every file's SHA-256 and the SHA-256 of the sorted `hash  path\n` inventory;
`upstream-files.json` preserves the corresponding upstream Git blob hashes.
No build downloads source. The release snapshot was read using GitHub's API
because shell network access was unavailable. This is not a re-created release
tarball. The vendored source is sufficient to rebuild the shipped compiler.

TCC is LGPL-2.1-or-later; retain `COPYING` (also `upstream/COPYING`). Individual
source notices refer to version 2 or later; the supplied upstream licence text
is LGPL 2.1. Runtime `libtcc1.c` grants the additional permission to link the
runtime into programs without restriction; see its original notice and the
retained GPL-2.txt. The runtime's four integer division/remainder exports are
weak in the Ciuki patch so the existing SDK helpers can coexist with TCC's
floating conversion and shift helpers.

The target compiler links `apps/tcc/capture.c` as a private newlib `_write_r`
wrapper. It can write stdout/stderr to CiukIDE's existing message channels;
ordinary file/console output falls back to the SDK hook. The current SDK has
no POSIX pipe implementation, so this adaptation adds no kernel syscall.

`patches/0001-ciukios-static.patch` is the CiukiOS modification, applied only in
the build directory. It selects the existing CiukiOS newlib CRT and libraries,
static page-separated RX/R/RW ELF output, non-executable stack, and disables
in-process execution and dynamic linking. The same source builds the on-target
compiler and the host cross compiler. Modified-source notices and this patch
are distributed with the repository. `/System/sdk/TCC.TXT` includes this notice
and the complete LGPL text. SDK/newlib and libdeskui retain separate licences.
