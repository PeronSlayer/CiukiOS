# Unmodified unified DOS inputs (f3-55)

The three ZIP files here are the untouched nested `SOURCES.ZIP` archives from
the official FreeDOS 1.3 kernel, FreeCOM and SHARE packages. They retain their
authors, original file notices, GPL and SUPPL LGPL texts. These are source
inputs, not a claim that all files have the same licence. Exact outer-package,
nested-archive and per-file pins are checked by `scripts/unified_dos.py`
against `../1.3/UPSTREAM.json` and `docs/design/unified-dos-licenses.tsv`.
The ledger is authoritative for the exact 181/800/2 source member lists.

| Input | SHA-256 |
| --- | --- |
| kernel-2043-sources.zip | a977497b7d1b2990dc50542cf1a8da2965079f74c37508d5894ecc02a4c8a9d5 |
| freecom-0.85a-sources.zip | cdd1702c137037393f14adad64d4c3fe5e87d7381d1260c1be12b74f65d8e087 |
| share-2006a-sources.zip | 2e28bf858e38086764d4b237ffd0273d687fcb6f8d970c1887559b63511ce7a3 |

The original outer packages remain downloaded, ignored inputs under `../1.3/`.
No code is taken from either master checkout. Open Watcom 1.9's installer,
source, license and Linux tool hashes and the host environment are pinned in
`config/openwatcom.json`. Extracted tools, source trees, maps and binaries stay
inside this worktree's `build/`.

Run `tests/host/dos/check_unmodified_build.py` as documented in its docstring
and `docs/validation/f3-55-unmodified-kernel.md`. Native OW1.9 execution is
required: no emulator or compiler fallback. The kernel is **unmodified except
patch 0001 (reproducible date)**, documented and SHA-pinned under
`ciuki/patches/` and `ciuki/build.json`. The external generated forced include
`ciuki-build-date.h` defines the fixed K2043 release date `May 14 2021` and
`00:00:00`; `config.mak` passes the space-free `-fi=../ciuki-build-date.h` token
to both kernel and SYS compilation. Every source file is hashed
before/after the patch and after compilation. The guest kernel may
link only source-built kernel/device objects, never a Watcom runtime library.

After host qualification under different host dates, roots and input mtimes,
the canonical builder can install the new opt-in
`UNIFIED.IMG` through the same FAT16 SYS/MBR path as the reference. It keeps
FreeCOM 0.85a, SHARE and DOS=LOW with the DFVM bridge disabled. The old guest
and pristine reference retain their pins, defaults and overlay namespaces.
Amendment 3 requires the lead's f3-48 differential battery at 100% identical
results before behavioural acceptance. Vendor byte comparison is superseded;
UPX pin/unpack tooling is removed. Raw size/section layout is recorded beside
the packed vendor hash, without claiming equivalent binary representations.
The source ZIP is placed beside the output image. Source URL publication is
pending the lead's immutable URL and verification; no live offer is claimed.
