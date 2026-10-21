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

Amendment 2, item 4 (lead decision on the packed vendor kernel): the pinned `KERNL386.SYS` is UPX-packed (`UPX!` at offset 85, 46,256 bytes) while the supported rebuild is uncompressed. Decision: the lead pins **UPX** (latest upstream Linux release from github.com/upx/upx, archive and SHA-256 in `build/downloads/upx/`, record it in `config/upx.json` with its licence (GPL-2.0 with the UPX exception) in the inventory); the harness produces the **unpacked vendor representation** with `upx -d` on a copy, records its SHA-256 as provenance beside the pin, and the masked comparison (date string region excluded) runs between that unpacked vendor kernel and the raw rebuild. Packing the rebuild is not required. The design note says explicitly which representation each hash refers to.

## Amendment 3 (lead, 2026-10-21): the lead's native build after amendment 2, and the vendor-comparison decision

1. **Build break (lead run, `build/lead/h55-a2/make.log`):** the date macros are passed as `-dCIUKI_BUILD_DATE="Oct 21 2026"`; wmake/wcc split the quoted value at the spaces and `wcc` fails with `E1139: Command line contains more than one file to compile` on every SYS source. Pass the macros through the forced-include header (`-fi=…/ciuki-build-date.h` defining `CIUKI_BUILD_DATE`/`CIUKI_BUILD_TIME`: the mechanism failed before only because it redefined `__DATE__`, which patch 0001 no longer needs) or as space-free tokens; the values are **fixed** (the kernel 2043 release date, from the pinned manifest), not the wall clock, so the two-date check compares builds whose only difference is the host clock. Everything up to SYS already builds (boot sectors, device.lib): keep going to `KERNL386.SYS`.
2. **Decision — vendor byte comparison replaced:** UPX 5.2.1 reports `NotPackedException: not packed by UPX` on the vendor `KERNL386.SYS` (a flattened SYS image around a packed EXE); reconstructing the wrapper is not worth its fragility. The acceptance becomes: (a) reproducibility, two builds under different host dates byte-identical; (b) **behavioural equivalence**, the f3-48 differential conformance battery run with the rebuilt kernel against the pinned vendor kernel as the reference, 100 % identical results (the lead runs it on QEMU, allowed for this battery); (c) size and section layout of the rebuild recorded beside the vendor file's hash as provenance. Remove the "blocked" status for the masked comparison and document the reason; keep the UPX pin (it is used nowhere else: drop `config/upx.json` and the unpack tooling if nothing else needs them, say so in the report).
3. Apply this amendment only; report section "Amendment 3".

## Amendment 4 (lead, 2026-10-21): the lead's native build after amendment 3

Compilation of SYS now succeeds; the link of `sys.com` fails (`build/lead/h55-a3/make.log`): `wlink` reports `W1107: file __wcl__.lnk: line(2): undefined system name: com` and `E2010: I/O error processing : Is a directory`. The Open Watcom linker resolves `system com` through `wlsystem.lnk` (`binl/wlsystem.lnk` or `binw/wlsystem.lnk` in the installer), located through the `WATCOM` environment variable (`$WATCOM/binl/wlsystem.lnk`) or `WLINK_LNK`; the extraction does not include that file and the build environment does not set `WATCOM`. Extract `wlsystem.lnk` (hash-listed), set `WATCOM` (and `INCLUDE`/`LIB` as upstream `owlinux.mak` expects, `EDPATH` if needed) in the fixed build environment, and make the dependency-completeness test cover the linker files too. Apply this amendment only; report section "Amendment 4".
