# Setup native integration

## Research and implementation decisions

The f9-04b directive approves the following integration boundaries.

- [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf), sections 4 and 6: retain the real short alias and all ordinary FAT attribute bits. Expose metadata and cluster addresses through a bounded descriptor query, restricted to the Setup capability in Setup mode. Check the mount and chain generation on every continuation; reject stale descriptions. Attribute updates cannot turn files into directories or volume labels.
- [ATA/ATAPI-6 revision 3a](https://flint.cs.yale.edu/cs422/readings/hardware/ATA-d1410r3a.pdf), FLUSH CACHE: retain libdisk's write/flush/raw-read ordering and final MBR publication barrier.
- [dosfstools 4.2](https://github.com/dosfstools/dosfstools/blob/v4.2/src/mkfs.fat.c): reuse the existing formatter and independent fsck/mtools oracle.
- [El Torito 1.0](https://www.ardent-tool.com/storage/specscdrom.pdf), hard-disk emulation, and [SeaBIOS 1.16.3](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/cdrom.c): loader source remains the verified boot-image bytes at byte offset 512. No second optical reader is introduced.
- [ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/): preserve the existing ISO tree and manifest translation. Embedded manifest SHA-256 identifies the medium; the completed ISO hash stays external.
- [DS12885/DS12887 vendor manual](https://www.analog.com/media/en/technical-documentation/data-sheets/DS12885-DS12C887A.pdf), time/calendar and register B: serialize CMOS writes with SET, retain binary/BCD and 12/24-hour mode, preserve interrupt controls and NMI policy, and verify read-back. UTC kernel time and the stored RTC convention are distinct.
- Windows 98 Resource Kit, locally available read-only chapter 10 (`wrkc10_1.htm`, table 10.2; `wrkc10_5.htm`, partition/Format; `wrkc10_6.htm`, block drivers, FAT32 and cache/error handling): retain the existing disk tools and durable phase markers; no source text is redistributed.

Repository comparison: f9-03 requires graphics grants and power authority; f9-04's native preflight, metadata and configuration hooks are stubs. The SDK recipe omits libsetup. The main checkout contains f2-100's checksummed NETWORK.INI codec; this branch predates it. Reuse that exact codec/format and document its integration overlap. The current clock page changes presentation only, so shared installation configuration writers and a checked RTC boundary are required.

## Interface amendment

Setup transaction interface version 2 adds `erased_total` and `payload_bytes`.
`erased_partitions` is the represented prefix (at most 16); `erased_total` is
the complete count. The wizard shows the prefix and "and N more".
`required_bytes` is libdisk's inclusive total; payload, overhead and reserve
are breakdowns. `source_iso_sha` names the embedded CDMANIFE.JSN SHA-256.

The manifest carries numeric attributes, directories and the exact canonical
disk BOOT.CFG text and SHA-256. Setup validates that template before writing
the destination, and never tries to recover discarded trailing whitespace
from the Setup-mode options.

## Native integration details

The native adapter maps C: to `/` and the other letters to `/mnt/<letter>`.
It retains a file description and its mount/chain generations during raw
verification. A request walks at most 128 links; sequential verification
continues the descriptor cursor. The read-only query also exposes the real
8.3 alias, which native verification compares with the source alias using DOS case
insensitivity (the NTRes display-case flags do not change an 8.3 identity). Source
ISO records use their ISO names and numeric attributes. Structural FAT bits
cannot be changed through the attribute writer.

P2 now creates `System`, the canonical display spelling. Its short alias is
still SYSTEM and the loader's paths are unchanged. This prevents the new
manifest's directory-case verification from rejecting P2's own directory.

`cfg_write_at` is the common durable writer in `apps/desktop/settings_io.c`.
Control Panel's existing asynchronous writer delegates to it; Setup calls it
with the target prefix, without starting a worker. Staging is committed
before rename and the installed file is synced. A rename EIO preserves the
old canonical file; the existing non-replacing fallback is limited to EEXIST.
The offline registry merge uses libreg's parser/serializer and preserves
existing keys. The destination lease and the unpublished MBR cover offline
configuration; no live registry service or network connection is started.

The stores are:

- `System/Registry/System.reg`: `System\Identity` UserName/ComputerName;
  `System\TimeZone` OffsetMinutes/RTCLocal. Existing identity/brand keys stay.
- `system/settings/desktop.cfg`: existing Control Panel keyboard encoding.
- `SYSTEM/NETWORK.INI`: the unchanged f2-100 codec, checksum and physical
  PCI/MAC identities. A restricted read-only query supplies at most two
  identities. Static configuration applies to en0; other adapters retain
  DHCP defaults. None disables both. IPX/lobby persistence is deferred.
- `System/Registry/Programs.reg`: `Programs\BundledGames\<package>` with
  Installed, Name, Command and Image for the three bundled shareware titles.
- `system/settings/clock.cfg`: exact LF lines `version=1`, `rtc_local=0|1`,
  `offset_minutes=<signed decimal>`. Boot subtracts the offset from a local
  RTC sample while the native realtime clock remains UTC. The checked RTC
  writer preserves its binary/BCD and 12/24-hour conventions. Its year range
  is the existing reader's 1980–2079 pivot; no century CMOS byte is guessed.

The f2-100 codec was read from the main checkout and reused without changing
its grammar. Integration with main must keep one definition of its private
identity type and codec declarations (`net_config.h` / `net_control.h`).

## Qualification boundary requiring lead attention

The retained f9-01 `cdview.c` deliberately quarantines an old C: view for the
rest of the boot when the optical generation changes. It currently has no
rebind path. This is safe against stale media, but libsetup's matching-disc
Retry model assumes that C:/D: can be reopened on replacement media. The lead
must reconcile a verified rebind with that sticky-quarantine policy, or make
a C: source-view loss a restart-required outcome. This integration does not
silently relax generation checks. Stable-media native installation is covered
by the file-backed model; native physical reinsertion remains unqualified.

## Host evidence

All test jobs ran serially with `nice -n 19`; no image/guest build or execution.

| Check | Result |
| --- | --- |
| CD media producer/model | 15 tests (one missing-canonical-manifest skip), 725 kernel-media checks; exact template/hash, directories, attributes and reproducible fixture media. |
| Filesystem runner | Full pass, including 1,058,011 libdisk checks, native raw boundary, crash/fsck/mtools checks; artifact budget 191,055,448 logical / 142,520,320 allocated bytes, limit unchanged at 200,000,000. |
| Final libsetup focus | 33,511 checks with the repository's ASan/UBSan policy; real native adapters/configuration writers over a file-backed model, System spelling, DOS alias case/mismatch, and 20-record preview. |
| Wizard | All 28 existing frame hashes and functional groups pass under UBSan; no-write navigation/confirmation gate retained. Default ASan/LSan exits at the sandbox's ptrace restriction after functional assertions. |
| Kernel boundaries | UBSan: supervisor 303,639 checks, RTC 6,370, native files 16,196, power zero failures. |
| ABI | Target/i386 compilation including SDK offsets; 1,793 target/native oracle checks pass under UBSan. Default native sanitizer exit is blocked by LSan's ptrace requirement. |
| Production linkage | Seven-member libsetup plus real libdisk/libreg/libdeskui, complete i686 wizard ELF inspection, no undefined or duplicate external library symbols; nine kernel units pass canonical target compilation/frame limits. |
| Shared writer | Existing settings worker regression plus actual prefixed-path replacement, rename EIO preserving old data, and staging cleanup pass. |

Exact commands and limitations are retained in
`build/codex/f9-04b-report.md` and its small text logs. A guest resident-set
measurement and actual optical reinsertion/restart behavior are not qualified.
