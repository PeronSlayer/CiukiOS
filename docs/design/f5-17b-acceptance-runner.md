# F5-17B runner integration — 2026-10-20

The installer/title case kinds extend the canonical runner. They use its
existing per-boot overlay, scope verification, memory/swap ceilings, host
deadline, lock, serial collector, teardown and retention. No image rebuild or
guest personality change is involved. `--case <exact-id>` selects an authored
case; unknown IDs are refused and required smoke cases remain included.

Research checked the current implementation against these primary sources:

- [QEMU QMP input-send-event](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
  and [screendump](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-screendump):
  use qcode key, relative-axis and button events; capture PPM through QMP.
  Receipts are not guest response measurements. Poll named regions, preserving
  key-hold gaps and the last matched screen anchor through key-up events.
- [QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html): retain
  opt/it.alcybercloud.ciukios/test as the runtime selector channel. The kernel
  must register each probe; accepting a host request supplies no guest driver.
- [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
  preserve FAT16/FAT32 geometry and cluster-count bounds; normalize creation,
  access and write directory timestamps. Explicit FAT16 sectors-per-cluster
  keep the small fixture above the FAT16 minimum cluster count.
- [ECMA-119 fourth edition](https://www.ecma-international.org/wp-content/uploads/ECMA-119_4th_edition_june_2019.pdf):
  reuse the existing deterministic directory masterer for cooked data.
- [GNU libcdio CD image formats](https://libcdio.github.io/libcdio.html#CD-Image-Formats):
  cooked data and audio sectors are distinct. Attach only the mixed-mode
  manifest's actual data.path as ide-cd. Keep CUE/audio provenance explicit;
  the authored tone is test audio and supplies no original-title music proof.
  The f5-17A GNU ccd2cue MODE URL returned an error during this research;
  no new claim relies on retrieving that page.
- [Microsoft PE/COFF](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format):
  named/ordinal imports and export forwarders require actual export-table
  resolution. Reuse census.PE, retain static/delay import kinds, recurse through
  forwarders and distinguish present exports from classified implementations.

## Tier and evidence rules

| Tier | Required guest evidence | Independent host evidence |
| --- | --- | --- |
| installers | image/payload/scene identity; install segment and process start/exit; default choices, no system writes, menu/uninstall policy; installed name chunks; kernel start/exit | installer/desktop screen matches; verified file inventory; documented installer exit codes |
| titles | identity; start/play/exit, optional focus/uninstall; launch/name; ordered loader stages; mode, progress, frames/FPS, stream open/audio/input/commit; process and kernel exits; six ledgers | menu/play/desktop and selected focus regions; changing same-region progress pixels; reference audio; exact acknowledged latency input IDs |

Suite `expected.predicates` uses the existing shared where/fields/relations/
count evaluator. Win32 schema checks remain strict (240-byte records, bounded
count and sequence, exact run/probe and hash/scene identity). Inventory name
chunks are contiguous UTF-8; installed rows distinguish file/directory/registry/
link. Files carry sizes/digests; links also carry decoded targets. The manifest
currently provides only a future verified file list; complete original registry,
link and uninstall residue references remain f5-15B lead inputs.

New DATA `audio_open` fields: segment, pid, stream_id, rate, channels. The title
tier requires a play-segment stream open in addition to qualified/non-silent
consumption and host audio checks. `name`, `installed` and `launch` extend the
f5-17A draft protocol. Guest emission remains lead-owned and is not claimed.
Every parsed record gets a host timestamp in result.win32.record_host_times;
image/payload/scene and edition bindings live in the enclosing result. Guest
milliseconds and icount remain separate domains. Edition may remain null in
an authored case; live preflight requires an edition and reference-qualified
script status. Observed launch kind/path must agree with the declaration.

Pending pins/scripts/launch paths and missing/oversized guest probes produce
not_run before an overlay/fixture is created. Archive verification requires a
reviewed manifest SHA-256 before fixture generation; a SHA-1-only candidate is
never promoted automatically. Scanned trees reject symlinks, special files,
case collisions and the reserved CIUKI-ID.TXT name; payload identity hashes
sorted path/size/hash records. The source tree's identity is independent of the
archive hash; a reference install must still qualify that tree.

Each title has authored scripts and separate installer/title cases. Their keys,
regions and null pins are explicitly provisional, not observations. Focus
coverage follows the f5-17A proposal (Diablo/StarCraft/RCT); lead qualification
remains required. Titles currently launch a reference-installed fixture tree,
not the installer tier's discarded overlay. That distinction keeps the
whole-title lifecycle gate open.

The selector test audits every suite selector against selector.c, menu.inc and
loader_model.py. Its exact owner-labelled pending list fails on new omissions,
changed omissions or stale entries. It registers nothing. In particular,
w32-fixtures and w32-kernel32 are missing from the kernel/menu tables while
the loader model accepts them. Long StarCraft/Worms title probes exceed the
kernel's 23-byte name limit; aliases or a reviewed protocol change need the
lead. See the implementation report for the complete pending list.

## Census and operator workflow

`scripts/win32/acceptance_census.py --providers <builtin-folder> --coverage
<metadata.json> --output <report.json>` inspects extracted and installed
acceptance trees, and resolves named/ordinal/forwarded imports. Repeat providers
for distinct DLL stores; duplicate providers are refused. A builtin-store.json,
when present, binds provider hashes. Repeat coverage in replacement order:
generated stubs.json first, then authoritative per-DLL/facade coverage. Coverage
formats are the existing builders' exports lists, DLL/name maps and generated
stub tables. W24 needs an explicit owning directive. Missing exports or missing/
conflicting classification stay unresolved; export presence alone is insufficient.
The result records all provider/coverage hashes, every executable hash, resolution
counts and unresolved rows. Opaque children, dynamic imports and absent trees
remain separate open evidence, never inferred from static imports. W21 resolution
indicates an API stub, not successful runtime behavior.

`acceptance-resolution.json` is regenerated for the available acceptance inputs;
all ten are absent in this worktree. Zero imported functions observed means no
coverage measurement, and complete=false remains explicit. A final census cannot
be fabricated without original inputs and f5-15B install/play observations.

Each `scripts/win32/titles/<id>.py` prints its plan by default. The lead may
verify/fetch through the manifest downloader, master into build/test-runs or
build/host, or explicitly invoke the runner for one exact case. For example:

```sh
nice -n 19 python3 -B scripts/win32/titles/diablo-shareware.py --tier installers
nice -n 19 python3 -B scripts/win32/titles/diablo-shareware.py --verify
```

Downloads, extraction, original-title runs, canonical build, QEMU and hardware
qualification were not performed by this implementation. Evidence templates
are docs/validation/f5-acceptance-template.md and the explicitly canned
f5-17b-diablo-shareware.md example.
