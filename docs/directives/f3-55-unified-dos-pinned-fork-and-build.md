# f3-55 — Pinned FreeDOS 2043 fork and reproducible unmodified guest

- **Phase/model:** F3 unified DOS; Codex, gpt-6.1-sol, xhigh, internet research.
- **Status:** complete implementation directive text delivered by f3-54;
  launch after lead cross-review and the reservations below.
- **Worktree:** lead creates `wt/f3-55` from integrated main. Codex does not commit.
- **Authority:** [unified DOS contract](../design/unified-dos-contract.md),
  inventories and per-file license ledger; source/toolchain acquisition pins,
  narrowly scoped build/extraction scripts, native runtime personality selection,
  canonical fixture-install glue, source-offer/license installation, host tests.
- **Dependencies:** lead approves the contract, pins the exact OW1.9 installer
  and any redistributed tool source by SHA-256, and reserves the unified runtime
  guest/profile selector. No number is supplied by this draft. The old guest
  and pristine reference remain independently selectable and unchanged.
- **Size:** source/vendor/build integration plus host verification; no DFVM or
  file-service implementation in this phase. Boot evidence belongs to the lead.
- **ABI reservation:** the runtime guest/profile selector and any configuration
  schema value required to name the unmodified rebuilt guest are lead-assigned.
  No new DOS interrupt number or public syscall is requested in this phase.

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
2. Acquire exactly the reviewed `open-watcom-c-linux-1.9` asset from tag ow1.9
   and record its SHA-256, URL, size, license and extracted tool hashes. Use
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
   directories, with distinct absolute roots and input mtimes; compare raw
   kernel bytes, source hashes and manifests. Do not expect our uncompressed
   kernel to hash to the vendor's compressed KERNL386.SYS.
4. Inspect link maps and dependency closure. The Linux top-level empty libm
   and kernel-owned math helpers must avoid Watcom CRT/startup/library code
   in the guest. Record every linked object and its source/license. No opaque
   .LIB or incompatible runtime enters the GPL guest. If a support object is
   required, obtain a compatible implementation/permission before distribution.
5. Add an opt-in source-built guest alongside the existing CiukiDOS and
   pristine FreeDOS reference, using the pinned FAT16 SYS boot path and actual
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

## Acceptance and host tests

| ID | Required evidence |
| --- | --- |
| H55-01 provenance | Every outer/inner/member hash, version and license matches; corrupt/missing/traversal/duplicate/archive-bomb fixtures rejected without partial import. |
| H55-02 tools/build | Tool/version hashes and Linux execution, two byte-identical unmodified builds across roots/mtimes; timestamp behavior explained; complete link-map source/license closure. |
| H55-03 boot layout | Production host CPU/boot-template checks cover MBR/FAT16 loading and entry/relocation bounds for the rebuilt kernel; old guest and pristine reference unchanged. Do not label host instruction execution a full VM boot. |
| H55-04 selector | Lead-reserved selector parsed/read back; default remains old; independent overlays by guest/base content; invalid/missing/corrupt rebuilt guest fails locally and preserves configuration. |
| H55-05 distribution | Independently read back all installed licenses, source hash/URL and exact kernel/shell/profile identities; source archive contains enough scripts/configuration and source for every shipped guest object. |

Implement new provenance/build/selector unit tests in tests/host/dos/ and a
focused host build checker with its exact documented command. Run sequentially:

```sh
nice -n 19 python3 -B -m unittest discover -s tests/host/dos -p 'test_*.py' -v
nice -n 19 bash tests/host/dos/run_file_api.sh
nice -n 19 bash tests/host/dos/run_hook_cpu.sh
```

Run the new unmodified-kernel host build checker twice as H55-02 specifies.
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

## Amendment 1 (lead, 2026-10-21): the forced-date probe fails on the supported host; minimal reproducibility patch

The lead ran `tests/host/dos/check_unmodified_build.py` outside the sandbox: the pinned native Open Watcom 1.9 Linux tools execute (the sandbox SIGSYS was an environment limit), but `wcc` rejects the forced include that redefines `__DATE__`/`__TIME__` (`E1100: Definition of macro '__DATE__' not identical to previous definition`, logs `build/lead/h55-a.log`, `h55-b.log`). Decision: do not fight the compiler; apply the **minimal documented reproducibility patch** the contract foresaw:

1. In the fork, replace the `__DATE__`/`__TIME__` uses (`hdr/version.h:48` and any other site; FreeCOM's `__DATE__/__TIME__` likewise when f3-58 builds it) by build-provided macros (`CIUKI_BUILD_DATE`, `CIUKI_BUILD_TIME`) passed on the command line from the pinned manifest's date (fixed, documented, e.g. the kernel release date), with the upstream default preserved when the macros are absent. Record the patch as a numbered patch file under the fork's `ciuki/patches/` with its rationale and SHA-256; the design note and the validation record call the result "unmodified except patch 0001 (reproducible date)".
2. The pristine comparison: build with the pinned toolchain and compare to the pinned `KERNL386.SYS` byte for byte **outside the date string region** (locate the string in both binaries; everything else must match: this is the proof that the toolchain and sources reproduce the vendor build). Then the two-date comparison (`--mtime 946684800` and `1609459200`) must produce identical binaries.
3. The lead reruns `check_unmodified_build.py` on the host; make its output name the patch and the masked comparison explicitly. Report section "Amendment 1".

## Amendment 2 (lead, 2026-10-21): remaining findings of the Codex cross-review of de75a586 (finding 1 is Amendment 1)

Apply after Amendment 1.

1. **Toolchain extraction incomplete (High):** `scripts/unified_dos.py:168` extracts only `binl/` and `license.txt`, but `make all` of kernel 2043 builds SYS first, which needs the Watcom headers (`h/`: `stdlib.h`, `dos.h`, …) and the DOS libraries (`lib286/`) for the `wcl` link. Extract and verify (by hash list) the needed `h/` and `lib286/` subsets from the same pinned installer under `build/tools/openwatcom-1.9/`; keep the prohibition on linking Watcom runtime objects into the guest kernel itself (SYS is a host-side DOS utility, document which outputs may link the runtime); add a dependency-completeness test.
2. **Selector not reachable through the production loader (High):** `src/kernel/probes/selector.c:98` accepts the new selector, but `dos-conformance` is missing from the loader table (`src/boot/ciukldr/menu.inc:312`) and the loader model (`scripts/test/loader_model.py:11`); the loader's F3 suffix parser accepts only `platform=e500`; `tests/host/dos/protocol.py:9` rejects `kernel=unified`. Register the probe and the exact `kernel=unified` grammar in loader, model and host protocol, preserving every existing selector; tests passing the complete selector through all three paths.
3. **Tracked absolute symlink (Medium):** `third_party/freedos/freedos` was the lead's provisioning mistake and is removed by the lead; never re-add it; the FreeDOS reference resolves through `third_party/freedos/1.3` or skips with the reason.

Report section "Amendment 2".
