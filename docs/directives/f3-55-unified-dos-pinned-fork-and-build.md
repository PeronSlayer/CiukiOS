# f3-55 — Pinned FreeDOS 2043 fork and reproducible unmodified guest

- **Phase/model:** F3 unified DOS; Codex, gpt-6.1-sol, xhigh, internet research.
- **Status:** complete implementation directive text delivered by f3-54;
  launch after lead cross-review and the reservations below.
- **Worktree:** lead creates `wt/f3-55` from integrated main. Codex does not commit.
- **Authority:** [unified DOS contract](../design/unified-dos-contract.md),
  inventories and per-file license ledger; source/toolchain acquisition pins,
  narrowly scoped build/extraction scripts, native runtime personality selection,
  canonical fixture-install glue, source-offer/license installation, host tests.
- **Dependencies:** Amendment 1 confirms the contract choices below. The lead
  downloads and pins both selected OW1.9 assets before tool execution. This
  unmodified-upstream phase may start before the owner's answer on ported-code
  rights and the corresponding-source destination; distribution needs that
  destination. The old guest and pristine reference stay independently selectable.
- **Size:** source/vendor/build integration plus host verification; no DFVM or
  file-service implementation in this phase. Boot evidence belongs to the lead.
- **ABI:** the lead has reserved the exact BOOT.CFG/DOS Box `kernel=unified`
  token and per-game kernel-choice schema value **2**. Extend the existing
  `kernel` setting; preserve `ciukidos` and the pristine reference's current
  token/value and default. No new DOS interrupt or public syscall is requested.

## Read and research

Read AGENTS.md, f3-54 in full, both inventories, UPSTREAM.json, current
build_dos_image/dos_conformance/reference boot paths and native guest selection.
Read the pinned K2043 makefiles, config.m, hdr/version.h, boot/boot.asm and
the official [2043 release](https://github.com/FDOS/kernel/releases/tag/ke2043),
[OW1.9 release](https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9)
and [compiler documentation](https://open-watcom.github.io/open-watcom-1.9/c_readme.html).
Use rg/sed and bounded source reads. Never substitute master for release source.

## Required implementation

1. Pin outer kernel package, nested SOURCES.ZIP, member list and every source
   hash to f3-54's ledger. Import an untouched upstream source archive with
   notices/authors under a documented third-party location and keep build
   extraction/output under build/. Validate archive names, duplicates, traversal,
   absolute paths, symlinks, size limits and pin mismatches before extraction.
   No code is copied from the unpinned kernel-master directory.
2. Consume the lead-pinned `open-watcom-c-linux-1.9` and
   `open-watcom-1.9.0-src.tar.bz2` from the official tag ow1.9, using the
   `config/openwatcom.json` schema below. Reject absent/unfilled/incorrect pins
   before execution or extraction. Record license and extracted tool hashes. Use
   native Linux wmake/wcc/wlink/wlib; prove required 32-bit host execution works.
   Missing host support is a local error with evidence, not a DOS/QEMU compiler
   fallback. Pin NASM3.02 and host compiler environment too. Do not silently
   select a daily OW2 or GCC variant.
3. Build a genuinely unmodified K2043 extraction using Linux make all,
   BUILDENV=linux/XCPU=386/XFAT=32/LOADSEG=0x60 and no UPX. config.mak and
   environment are external declared inputs. Audit hdr/version.h's __DATE__:
   freeze compiler predefined date/time via supported build configuration
   if possible and prove it affects no source bytes. Otherwise report a
   reproducibility blocker before making a source patch. Build twice in fresh
   directories, with distinct absolute roots, input mtimes **and different
   wall-clock calendar dates observed by the compiler**. Record both observed
   dates plus the fixed source epoch. Use a narrowly scoped host clock fixture
   if necessary; do not change the workstation clock. An unfrozen __DATE__/
   __TIME__ control must differ across the two clocks, while the frozen builds
   must match. An environment SOURCE_DATE_EPOCH or mtime-only comparison does
   not prove OW honors it. Compare raw kernel bytes, source hashes and manifests.
   Do not expect our uncompressed kernel to hash to the vendor's compressed
   KERNL386.SYS.
4. Inspect link maps and dependency closure. The Linux top-level empty libm
   and kernel-owned math helpers must avoid Watcom CRT/startup/library code
   in the guest. Record every linked object and its source/license. No opaque
   .LIB or incompatible runtime enters the GPL guest. If a support object is
   required, obtain a compatible implementation/permission before distribution.
5. Add the opt-in `kernel=unified` guest (per-game choice value 2) alongside
   existing CiukiDOS and the pristine FreeDOS reference, using the pinned
   FAT16 SYS boot path and actual
   FreeDOS relocation. Do not forge a CIUKIDOS v2 header. Use the same canonical
   image builder, runtime selection and separate kernel/base-hash COW identity;
   never overwrite or reuse old overlay metadata. Binding stays disabled for
   this unmodified milestone. Pin each guest's actual shell/SHARE/profile.
6. Add read-back provenance and notice installation. Preserve existing
   reference pins; identify rebuilt binaries separately. Install source/credits
   and license metadata under system/licenses/ and construct the complete
   corresponding-source archive beside the release artifact, with a lead-supplied
   immutable URL and hash. Never claim a source URL is live without checking.
   Keep source-only import and generated binaries out of unrelated directories.

## Lead-owned Open Watcom pin schema

The lead downloads both files into `build/downloads/openwatcom/` and writes
`config/openwatcom.json`. This is its normative schema/template (not a file to
create in f3-54); the two null SHA-256 fields must be filled by the lead:

```json
{
  "schema_version": 1,
  "toolchain": "Open Watcom C/C++",
  "version": "1.9",
  "release_url": "https://github.com/open-watcom/open-watcom-1.9/releases/tag/ow1.9",
  "license": "Watcom-1.0",
  "download_directory": "build/downloads/openwatcom",
  "installer": {
    "filename": "open-watcom-c-linux-1.9",
    "url": "https://github.com/open-watcom/open-watcom-1.9/releases/download/ow1.9/open-watcom-c-linux-1.9",
    "size_bytes": 83959748,
    "sha256": null
  },
  "source_archive": {
    "filename": "open-watcom-1.9.0-src.tar.bz2",
    "url": "https://github.com/open-watcom/open-watcom-1.9/releases/download/ow1.9/open-watcom-1.9.0-src.tar.bz2",
    "size_bytes": 27927922,
    "sha256": null
  }
}
```

All shown keys are required; reject duplicates, additional keys, wrong types
(including booleans as integers), schema versions other than integer 1, or
changes to the fixed strings/sizes above. In a usable pin file each sha256 is
exactly 64 lowercase hexadecimal characters, not null/empty/MD5. Treat filenames
as basenames under the fixed directory, never executable paths from the network.
Hash and size-check both artifacts before extraction/install; no implicit
download/latest fallback. Record per-member extraction and executable hashes
in the generated build manifest alongside host compiler/libc, NASM, environment
and build configuration. The official release's MD5 is only a cross-check.
The schema's version is unrelated to the per-game kernel-choice value 2.

## Acceptance and host tests

| ID | Required evidence |
| --- | --- |
| H55-01 provenance | Every outer/inner/member hash, version and license matches; corrupt/missing/traversal/duplicate/archive-bomb fixtures rejected without partial import. |
| H55-02 tools/build | Tool/version hashes and Linux execution; two byte-identical unmodified builds across roots/mtimes and different compiler-observed wall-clock calendar dates, with an unfrozen __DATE__/__TIME__ control proving those clocks differ. Fixed epoch/configuration and unchanged sources recorded; full link-map source/license closure. |
| H55-03 boot layout | Production host CPU/boot-template checks cover MBR/FAT16 loading and entry/relocation bounds for the rebuilt kernel; old guest and pristine reference unchanged. Do not label host instruction execution a full VM boot. |
| H55-04 selector | BOOT.CFG and DOS Box round-trip exact `kernel=unified`, per-game choice 2; existing old/reference tokens and defaults unchanged. Independent overlays by guest/base content; invalid/missing/corrupt rebuilt guest fails locally and preserves configuration. |
| H55-05 distribution | Independently read back all installed licenses, source hash/URL and exact kernel/shell/profile identities; source archive contains enough scripts/configuration and source for every shipped guest object. |

Implement new provenance/build/selector unit tests in tests/host/dos/ and a
focused host build checker with its exact documented command. Run sequentially:

```sh
nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v
nice -n 19 bash tests/host/dos/run_file_api.sh
nice -n 19 bash tests/host/dos/run_hook_cpu.sh
```

Run the new unmodified-kernel host build checker twice as H55-02 specifies.
Test pin parsing with each null/missing/malformed hash, changed URL/name/size,
duplicate/unknown field and corrupt/missing artifact; no installer executes.
Native selector/build integration also requires
`nice -n 19 bash scripts/test/host_kernel_tests.sh`; changed filesystem glue
requires `nice -n 19 bash scripts/test/host_fs_tests.sh`. If a public header
changes, regenerate API docs and run `nice -n 19 make CAP= apps`. Preserve each
script's sanitizer/leak settings; do not add a detect_leaks=0 override.

## Evidence and lead validation

Write build/codex/f3-55-report.md and a durable validation note: exact commands,
pins, artifacts, two-build comparison, map audit, H55 statuses and remaining
runtime evidence. Lead performs one canonical build, static image checks and
same-image runtime boot of all three guest identities under the authorized
DOS-conformance exception. No default switch. Measure conventional memory
and largest MCB for later comparison; no cap increase.

**Constraints:** no QEMU, systemd-run, OS image builds, commits/merge/push or
diary edits by Codex; host tests one at a time at nice19. No DFVM/FCB/NLS or
file backend implementation here; no new panic path, game patch, copied full
image, private-library disclosure or automatic old-guest retirement.
