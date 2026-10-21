# F3-55: unmodified source-built FreeDOS 2043 guest

Host implementation record, 2026-10-21. Runtime qualification, canonical image
construction and publication belong to the lead. No QEMU, systemd scope,
canonical image build or physical-memory qualification was run here.

Research used the [official K2043 release](https://github.com/FDOS/kernel/releases/tag/ke2043),
the [Open Watcom 1.9 release](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9),
its [getting-started documentation](https://open-watcom.github.io/open-watcom-1.9/c_readme.html)
and [compiler user guide](https://open-watcom.github.io/open-watcom-1.9/cguide.html).
The actual inputs are the literal nested source archives from the pinned
FreeDOS 1.3 packages, checked against the f3-54 per-file ledger. The release
page distinguishes the 386/FAT32 package binary from the other release builds;
the rebuilt uncompressed output is not expected to equal compressed KERNL386.SYS.

Relevant K2043 sources are `makefile:74–122`, `config.m`,
`mkfiles/{generic,watcom,owlinux}.mak`, `kernel/makefile`, `drivers/makefile`,
`hdr/version.h:48` and `boot/boot.asm:29–59,100–102`. The supported Linux path
uses GNU make, native wmake/wcc/wlink/wlib, host GCC for utilities and NASM.
The top-level makefile creates an empty libm; kernel-owned assembly implements
the required arithmetic. Link qualification rejects any unknown object or
library and records source hashes and licences for contributing objects.

Build configuration is external: `BUILDENV=linux`, `XCPU=386`, `XFAT=32`,
`LOADSEG=0x60`, no UPX, no debug variant. Open Watcom's documented `-fi`
forced include supplies an external date header that undefines/redefines
`__DATE__` and `__TIME__`. A compiler probe must demonstrate the exact frozen
date in its object before the build proceeds. This is a candidate supported
configuration, **not proven here**, because native tool execution fails first.
All 181 upstream source hashes must remain unchanged after compilation.
No upstream timestamp patch is made or implicitly authorised.

`config/openwatcom.json` pins the installer (83,959,748 bytes,
`f7484be27eb70028010303fc16bb2acc5a785679567a568b940c28190ddbf3f3`),
the present source archive (27,927,922 bytes,
`6d303327988ee2dda60cfabebf3f45a9758aee4da117d41cf3153fccb7e5e4bf`),
all four Linux tools, the verbatim Watcom-1.0 notice, NASM 3.02 and the
host compiler/make/libc environment. Local source filename uses underscores;
the official release URL uses hyphens. The raw bytes and size match the
published asset coordinates. Tools remain build-time dependencies; no
Watcom CRT is authorised in the GPL kernel.

Run these host checks sequentially, at nice 19. Use fresh roots; the second
successful build must have a different actual wall-clock date as required
by f3-54 amendment 1. Changing an evidence date field does not qualify it.

```sh
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-a --mtime 946684800
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --out build/codex/h55-b-longer --mtime 1609459200
nice -n 19 python3 -B tests/host/dos/check_unmodified_build.py --compare build/codex/h55-a/build.json build/codex/h55-b-longer/build.json
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
Comparison checks raw bytes, source manifests, tool/host pins and link closure;
only then is `build/unified-dos/qualified.json` written.

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

Recorded host outcomes:

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

This local artifact has no assigned publication URL. Native tool execution,
different-date reproducibility, actual map closure, a green LSan run and
lead-owned canonical/runtime/memory evidence remain open.
