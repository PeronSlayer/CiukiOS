# F3-55: source-built FreeDOS 2043 guest, unmodified except patch 0001 (reproducible date)

Host implementation record, 2026-10-21. Runtime qualification, canonical image
construction and publication belong to the lead. No QEMU, systemd scope,
canonical image build or physical-memory qualification was run here.

Research used the [official K2043 release](https://github.com/FDOS/kernel/releases/tag/ke2043),
the [Open Watcom 1.9 release](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9),
its [getting-started documentation](https://open-watcom.github.io/open-watcom-1.9/c_readme.html)
and [compiler user guide](https://open-watcom.github.io/open-watcom-1.9/cguide.html).
The actual inputs are the literal nested source archives from the pinned
FreeDOS 1.3 packages, checked against the f3-54 per-file ledger. The release
page distinguishes the 386/FAT32 package binary from the other release builds.
Amendment 1 now requires the strict masked pristine comparison described below.

Relevant K2043 sources are `makefile:74–122`, `config.m`,
`mkfiles/{generic,watcom,owlinux}.mak`, `kernel/makefile`, `drivers/makefile`,
`hdr/version.h:48` and `boot/boot.asm:29–59,100–102`. The supported Linux path
uses GNU make, native wmake/wcc/wlink/wlib, host GCC for utilities and NASM.
The top-level makefile creates an empty libm; kernel-owned assembly implements
the required arithmetic. Link qualification rejects any unknown object or
library and records source hashes and licences for contributing objects.

Build configuration is external: `BUILDENV=linux`, `XCPU=386`, `XFAT=32`,
`LOADSEG=0x60`, no UPX, no debug variant. The lead ran the pinned native tools
successfully outside the sandbox, but the original forced include failed with
E1100 on `__DATE__`. Amendment 1 replaces that approach with the explicitly
authorised patch 0001 below. All 181 source hashes are recorded before and
after patching and must then remain unchanged during compilation.

`config/openwatcom.json` pins the installer (83,959,748 bytes,
`f7484be27eb70028010303fc16bb2acc5a785679567a568b940c28190ddbf3f3`),
the present source archive (27,927,922 bytes,
`6d303327988ee2dda60cfabebf3f45a9758aee4da117d41cf3153fccb7e5e4bf`),
all four Linux tools, the verbatim Watcom-1.0 notice, NASM 3.02 and the
host compiler/make/libc environment. Local source filename uses underscores;
the official release URL uses hyphens. The raw bytes and size match the
published asset coordinates. Tools remain build-time dependencies; no
Watcom CRT is authorised in the GPL kernel.

Run these host checks sequentially, at nice 19. Use fresh roots and the two
input mtimes specified by f3-55 Amendment 1. Wall-clock dates are recorded,
but this amended input-date comparison may run on one host calendar day.

```sh
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-amendment1-a --mtime 946684800
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-amendment1-b-longer --mtime 1609459200
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --compare build/codex/h55-amendment1-a/build.json build/codex/h55-amendment1-b-longer/build.json
nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v
nice -n 19 bash tests/host/dos/run_file_api.sh
nice -n 19 bash tests/host/dos/run_hook_cpu.sh
nice -n 19 bash scripts/test/host_kernel_tests.sh
nice -n 19 bash scripts/test/host_fs_tests.sh
python3 -B scripts/gen_api_reference.py
nice -n 19 make CAP= apps
```

The checker records blocked attempts in `build.json`; no binary or qualification
is published on failure. On success it executes the pinned MBR/SYS FAT16
loader and the real kernel's initial relocation with libx86emu, stopping at
upstream trace `3`, before `FreeDOSmain`. Its sparse synthetic sectors are
host CPU evidence only. It checks the complete kernel load at 0060:0000 and
relocated code/stack bounds. It does not claim a complete VM/DOS boot.
Comparison checks raw bytes, patched/pristine source manifests, patch/date
manifest, tool/host pins and link closure, and independently repeats the strict
masked pristine comparison for both kernels. Only after these and the CPU
checks pass is `build/unified-dos/qualified.json` written.

`kernel=unified` is schema value 2 in the per-game DOS Box settings. Existing
values and the default remain unchanged. In a conformance `BOOT.CFG` selector,
use `f3:dos-conformance run=12345678 guest=ciukidos kernel=unified`.
The base is `SYSTEM/DOS/UNIFIED.IMG`, checked against its own `UNIFIED.SHA`;
COW names contain game, kernel, whole-base digest and generation. Binding is
disabled. The milestone uses exact original FreeCOM 0.85a and SHARE 08/2006a
binaries with the reference's DOS=LOW profile. This is not the future branded
source-built shell or a DFV2 implementation.

The canonical builder adds the guest only when qualification exists and reads
back its kernel/shell/SHARE, profile, identity and all notices independently.
It writes the corresponding-source ZIP beside the canonical output, preserving
the three upstream source archives plus source/build/boot inputs and hashes.
An absent immutable publication URL is explicitly recorded; no URL is claimed
live. **Distribution remains blocked until the lead supplies and verifies that
URL and source hash.** The original reference pins and overlay metadata are
not reused. Exact command outcomes, including local blockers and runtime
items still required, are in `build/codex/f3-55-report.md`.

Recorded original de75a586 host outcomes (before Amendment 1):

| Check | Outcome |
| --- | --- |
| DOS unit discovery | 59 tests pass; 10 focused provenance/build/source tests also pass after the final source-bundle adjustment. |
| File API and existing production hook CPU scripts | Pass. |
| Filesystem host suite and `make CAP= apps` | Pass. A pre-existing worktree output symlink was removed without changing its target so NetSurf could build locally. |
| Kernel host suite | Stops at LeakSanitizer's sandbox ptrace restriction. No leak-detection override was added. |
| Focused native selector/COW | 106314 assertions pass, then LSan cannot complete in this sandbox. |
| Both clean OW build attempts | All four native Linux tools return SIGSYS (-31), before make. No rebuilt kernel/map or two-build/date qualification exists. |
| Packed vendor CPU-loader diagnostic | Reaches load and initial relocation with bad=0; LSan then fails. It is separate from rebuilt-kernel qualification and does not compare compressed input with uncompressed RAM. |

The review corresponding-source ZIP is 2,058,651 bytes, SHA-256
`67a7e77aa6bcba398e34747f6e4aeb7347def32eac33dc504c931288d8e57d0c`,
created/read back with:

```sh
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --source-out build/codex/ciukidos-unified-source.zip
```

This original local artifact has no assigned publication URL. Its hash is
historical; Amendment 1 adds the patch/manifest/rationale to corresponding
source and therefore changes the source archive hash.

## Amendment 1

The result is **unmodified except patch 0001 (reproducible date)**. The
[numbered patch and rationale](../../third_party/freedos/unified/ciuki/patches/README.md)
are pinned by `third_party/freedos/unified/ciuki/build.json`. Patch SHA-256:
`816cca4f5bd4542013073cbd60246b78e9342db200262cb6a941aafd543c8045`.
It changes only the date use in `hdr/version.h:48` and `sys/sys.c:989`, with
upstream `__DATE__` retained as the default when `CIUKI_BUILD_DATE` is absent.
There are no other timestamp macro uses in the pinned kernel source archive.
FreeCOM stays byte-identical here; its date/time patch belongs to f3-58.

The pinned date is `Oct 21 2026`, time `00:00:00`, UTC epoch 1792540800.
These retain the original f3-55 external input date and now reside in the
fork manifest. `config.mak` passes `CIUKI_BUILD_DATE`/`CIUKI_BUILD_TIME` using
the documented [OW1.9 command-line macro syntax](https://open-watcom.github.io/open-watcom-1.9/cguide.html#Preprocessor),
without redefining compiler predefined macros. A native object probe checks
both values before make; source postimages and the manifest hash are build
evidence and part of two-build qualification and corresponding source.
The external `MAKE` command also overrides wmake's `CFLAGST` with its original
flags plus those date macros, because `sys/makefile` does not use `ALLCFLAGS`.
This covers SYS's banner without another source change.

The masked comparison locates the unique FreeDOS compile-date banner in
the raw rebuild and the vendor representation described by Amendment 2
below, and excludes exactly the 11 date bytes. All other bytes,
total lengths and date offsets must match. Each comparison is recorded in
`build.json` and independently repeated before qualification. Missing or
ambiguous banners and unrelated date strings cannot produce a PASS.

Read-back of the actual pinned vendor member confirms 46,256 bytes, SHA-256
`932c0c155701eddb7b902f7269a1b2ce31f5c82a6dc195172f2336d18a74e1fb`,
`UPX!` at offset 85 and no visible compile-date banner. Comparing it as-is to
the configured uncompressed output cannot pass this gate. Amendment 2
authorises `upx -d` on a copy as the equivalent representation; its measured
failure and remaining lead decision are recorded below. No expanded mask or
packing of the rebuild is authorised. This remains a qualification blocker,
not evidence of a compiler mismatch. Raw two-date reproducibility can still
be checked independently; it never substitutes for this gate.

Host unit coverage uses synthetic binaries only to verify exact masking,
outside-region differences, missing/duplicate/moved banners, patch SHA/context/
postimages, source immutability during the build and rejected forged records.
GCC preprocessing verifies that absent macros preserve the original banner
and explicit build macros supply the fixed date; this is not native OW proof.
The checker prints patch 0001, the masked comparison and the two-date result
explicitly. The exact amended test results and lead commands are recorded in
`build/codex/f3-55-report.md`. Native OW builds, actual link maps, a green LSan
run and canonical/runtime/memory evidence remain lead-owned.

Amendment 1 host results, run sequentially with the scripts' original sanitizer
settings (no additional ASAN/LSAN environment overrides):

| Exact command | Result |
| --- | --- |
| `nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v` | PASS, exit 0; final run 63 tests in 47.297 s. |
| `nice -n 19 bash tests/host/dos/run_file_api.sh` | PASS, exit 0, including i686 compilation. |
| `nice -n 19 bash tests/host/dos/run_hook_cpu.sh` | PASS, exit 0; production monitor/file-owner CPU harness. |
| `nice -n 19 bash scripts/test/host_kernel_tests.sh` | BLOCKED, exit 1; LSan's sandbox ptrace restriction. No workaround applied. |
| `nice -n 19 bash scripts/test/host_fs_tests.sh` | PASS, exit 0; fsck/mtools oracles and 2,339 crash cuts, 0 unknown diagnostics. |
| `nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --source-out build/codex/f3-55-amendment1-source.zip` | PASS, exit 0; deterministic source construction/read-back. |

The amended corresponding-source archive is 2,066,234 bytes, SHA-256
`3d613da80360b4756ea39271fa3df2e12737b2f06c8e9ba01e1962beebb6f6bc`.
The two native `--out` commands and their `--compare` above were not run in
this sandbox; the lead executes them on the supported host. The unit fixture
verifies the current packed-vendor blocker, without claiming native compiler,
real link-map, rebuilt-kernel CPU or guest-runtime qualification. No public
header changed; API-doc regeneration and the app rebuild are not required by
this amendment. The source URL remains unassigned.

## Amendment 2

Scope: items 1–4 only, on top of `67e0ef5a` and the lead's item-4 directive
commit `2c26af51`. Patch 0001, the guest/profile/default/COW identities, pinned
reference packages, filesystem glue and public headers are unchanged.

### Watcom dependencies and the selector

The pinned K2043 `makefile:110–118` invokes `sys` before `kernel`.
`sys/makefile:9,54–55` compiles against `$(WATCOM)/h` and links tiny-model
SYS with `wcl -mt`. The pinned installer's `binl/wlsystem.lnk:217–222` searches
`lib286`/`lib286/dos` and supplies `cstart_t.obj` for COM output. These are
checked against the [OW1.9 release](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9)
and [compiler documentation](https://open-watcom.github.io/open-watcom-1.9/c_readme.html).
`config/openwatcom.json` now records individual size/SHA-256 pins for Linux
wcl and 113 support files: 110 top-level/common-system headers and
`lib286/dos/{clibs.lib,doslfns.lib,cstart_t.obj}`. Extraction validates all pins
before writing and rechecks installed bytes. A previously valid binl-only
extraction gains the two missing trees without replacing native tools;
corrupt existing files are rejected. The dependency-completeness test covers
the actual SYS headers, linker startup and corrupt/missing pins.

Generated SYS is a DOS build utility, permitted to link the Watcom startup
and C runtime for the build. It is not shipped as the guest kernel or used to
replace the exact reference boot template. `link_audit()` still forbids
Watcom runtime/math/startup in the guest kernel. Build records and the
two-build comparison include the support-file pins.

The production loader registers `dos-conformance` and matches exactly:

```text
f3:dos-conformance run=<8 hex> guest=freedos
f3:dos-conformance run=<8 hex> guest=ciukidos
f3:dos-conformance run=<8 hex> guest=ciukidos kernel=unified
```

No platform/safe/duplicate/trailing/unknown suffix is allowed for this probe,
matching the already committed native parser. Menu, BOOT.CFG and validated
fw_cfg use the same grammar; existing F3 platform overrides remain fw_cfg-only.
The host loader model and wire protocol accept the same three strings.
The protocol keeps its existing `(run, guest)` return interface; unified
remains the CiukiDOS guest envelope. Host tests assemble unchanged bounded
ranges of production `selector_validate` and its actual tables, execute them
with libx86emu and compare acceptance with the native C parser, model and
protocol. This is parser CPU evidence, not a VM boot. Every earlier F3 name
and its platform override are checked too.

`third_party/freedos/freedos` remains absent and is not recreated. Reference
lookup remains the versioned `third_party/freedos/1.3`; a missing reference
returns its explicit provisioning error. Tests cover both facts.

### UPX: pinned representation and measured blocker

The supplied archive is `build/downloads/upx/upx-5.2.1-amd64_linux.tar.xz`,
664,048 bytes, SHA-256
`402162aad30af47e60dbd767fb2e64ca394ace9727ba1f40283641f1d1b91657`.
The [official 5.2.1 release](https://github.com/upx/upx/releases/tag/v5.2.1)
identifies this statically linked Linux asset. `config/upx.json` records its
URL, size/hash, every extracted file hash and the
[GPL-2.0-or-later licence with the UPX exception](https://github.com/upx/upx/blob/v5.2.1/LICENSE).
The extracted executable is 639,228 bytes, SHA-256
`287b3dffe9dcafd8e366e162ac4ab41e5cf45a3c6768970256af0869288d84a1`.
The archive is not copied into tracked source or distributed with the guest.

The harness verifies the complete bounded tar member list, rejects unsafe
paths, duplicates, links/special files and bad sizes/hashes, then extracts
under `build/tools/upx-5.2.1/`. It invokes `upx -d` with a minimal environment
on a temporary copy of the pinned vendor bytes, deleting that copy afterward.
The original reference package, member and hash are preserved. A successful
unpack must match a separate unpacked size/hash pin before comparison.
Only that verified representation can reach the existing strict 11-byte date
mask; comparison evidence carries its UPX/tool and packed/unpacked provenance.
Corresponding source includes `config/upx.json`.

Actual direct unpack returns exit 2, `NotPackedException: not packed by UPX`.
The [UPX manual](https://github.com/upx/upx/blob/v5.2.1/doc/upx.pod)
documents `-d` for supported formats; its DOS/SYS limit is about 65 KiB.
The pinned K2043 `utils/exeflat.c:218–238,402–448,470–474` has a distinct
large-kernel path: pack a temporary EXE, flatten it again, then add a custom
32-byte CONFIG wrapper and a trailer (`:272–348`). This source-backed
explanation is consistent with the actual vendor wrapper/marker and UPX's
rejection. No reconstruction or alternate decompressor was authorised here.

Representation hashes must not be conflated:

| Representation | Size | SHA-256 / status |
| --- | --- | --- |
| Original packed `BIN/KERNL386.SYS` | 46,256 | `932c0c155701eddb7b902f7269a1b2ce31f5c82a6dc195172f2336d18a74e1fb` |
| Direct `upx -d` vendor representation | unavailable | Explicit null pin; blocked by NotPackedException, not copied from the rebuild or RAM |
| Raw rebuilt kernel, patch 0001 | requires native build | Separate build-manifest hash; never compared directly to the compressed member |

Exact verification outcomes and remaining lead decisions are in the report
`build/codex/f3-55-report.md`; the real unpack diagnostic is retained in
`build/codex/h55-vendor-upx.json`. H55-02 cannot be qualified until the lead
resolves the unpacked-vendor representation and obtains native build evidence.

Amendment 2 verification (sequential, nice 19, original sanitizer settings):

| Exact command | Outcome |
| --- | --- |
| `nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v` | PASS, exit 0; 68 tests in 48.433 s |
| `nice -n 19 bash tests/host/dos/run_file_api.sh` | PASS, exit 0, including i686 compilation |
| `nice -n 19 bash tests/host/dos/run_hook_cpu.sh` | PASS, exit 0; production monitor/file owner, 0 unexpected traps |
| `nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p test_protocol.py -v` | PASS, 10 tests, including actual assembly/native parser and earlier F3 selectors |
| `nice -n 19 python3 -B -m unittest discover -s tests/host -p 'test_loader_*.py' -v` | PASS, 27 tests |
| `nice -n 19 bash scripts/test/host_kernel_tests.sh` | BLOCKED, exit 1, LSan sandbox ptrace restriction; no override |
| `nice -n 19 bash scripts/test/host_fs_tests.sh` | PASS, exit 0; 2,339 crash cuts, 0 unknown diagnostics, independent fsck/mtools checks |
| `nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-amendment2-a --mtime 946684800` | BLOCKED, exit 1; all five native OW tools SIGSYS (-31), before make |
| `nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-amendment2-b-longer --mtime 1609459200` | BLOCKED, exit 1; same native execution restriction |
| `nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --compare build/codex/h55-amendment2-a/build.json build/codex/h55-amendment2-b-longer/build.json` | Refuses qualification, exit 1; two successful builds required |
| `nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --unpack-vendor` | BLOCKED, harness exit 1 / UPX exit 2; NotPackedException |
| `nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --source-out build/codex/f3-55-amendment2-source.zip` | PASS, exit 0; construction/member read-back |

The amended local source ZIP is 2,077,810 bytes, SHA-256
`61c6c25c7764a488c1ec410d2257aee6666f26ec83c13174654dbf6aa45ebe51`.
It is a local review artifact, with no live source-offer URL claimed.
No public header changed; API-doc/app regeneration is not required here.
