# f2-123 hardware-name extraction and scoped public supplements

Validated on 2026-10-23 with host tests only. No image build, QEMU run or
hardware result is claimed. The f2-123 directive has no separate amendment in
this checkout; the associated video-family contract's lead decisions and owner
licence notes were read.

## Research and decisions

Microsoft's [INF Models documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-models-section)
permits literal descriptions as well as string tokens. Its
[Manufacturer documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-manufacturer-section)
identifies the model sections and their decorations. The locally readable
Windows 98 Resource Kit, Appendix C, `[Manufacturer]` and `[Manufacturer Name]`
sections (`wrkappc_4.htm` and `wrkappc_5.htm`), describes the same distinction.
The prior extractor required a token and scanned unrelated sections. The new
extractor accepts literal, quoted, token and mixed descriptions, resolves string
sections, preserves UTF-16 decoding, and limits rows to model sections.

The [PCI ID Repository](https://pci-ids.ucw.cz/) publishes name snapshots;
the [USB ID Repository](https://github.com/usbids/usbids) maintains the USB list
published at [linux-usb.org](http://www.linux-usb.org/usb.ids). Both formats
contain vendor/device hierarchies and independent class dictionaries, without
per-pair class annotations. The generator must not treat the class dictionaries
as device classifications. Public-source additions therefore use individual
class-labelled pairs in `assets/hwids/retro-scope.json`. Recognized Windows setup
classes select INF rows; mixed System-class rows require explicit bridge or
serial-bus function descriptions, or an individually classified pair. The
manifest includes NVIDIA/Ricoh cases whose INF names obscure those functions.

Direct `curl` requests to the named upstreams failed DNS resolution. Existing
[hwdata](https://github.com/vcrhonek/hwdata) copies supplied the original snapshots
and version headers. Their exact hashes are enforced in `scripts/build_hwids.py`
and recorded in `assets/hwids/README.md` and `docs/licenses/hwids-NOTICES.txt`.
The raw snapshots remain ignored under `build/downloads/`. hwdata's
[licence declaration](https://github.com/vcrhonek/hwdata/blob/master/LICENSE)
permits GPL-2.0-or-later; that option is used. Both upstreams are credited in
`docs/credits.md` and the About dialog, and the notice/GPL files are mapped into
`/system/licenses/` by the image builder.

INF labels have precedence, including explicitly scoped System rows. The eight
native reference labels fill gaps next; scoped public database labels fill the
remaining gaps. This preserves the established native NIC names and makes
AudioPCI supplements reproducible from pci.ids. Two display labels absent from
the pinned public snapshots are independently sourced:

- Intel `8086:00d1`: the upstream [Linux i740fb device table](https://cos.googlesource.com/third_party/kernel/+/2b8f68b99ba64d78e9109b4ddae1d2927e7e8623/drivers/video/fbdev/i740fb.c)
  explicitly combines the Intel vendor and PCI i740 ID.
- QEMU/Bochs `1234:1111`: QEMU v9.2.0 [PCI definitions](https://gitlab.com/qemu-project/qemu/-/blob/v9.2.0/include/hw/pci/pci.h)
  and [Bochs display model](https://gitlab.com/qemu-project/qemu/-/blob/v9.2.0/hw/display/bochs-display.c).

No driver code or Windows payload is imported.

## Measured output and coverage

| Table | Entries | Selected INF labels | Supplements | JSON bytes | SHA-256 |
| --- | ---: | ---: | ---: | ---: | --- |
| PCI | 1,284 | 1,213 | 71 | 66,743 | `7cdb0fdaf2ea1bbcec2ac44c53502dc2c36ed12fbfec10de49f8df8823dd0fa9` |
| USB | 234 | 218 | 16 | 12,562 | `0a19cdf87cdf6ceaad2231b6e1b6edbeb27b759c55034459e9f0aab4dedfbc7f` |

Read 909 INF files. All **404** Display model pairs are named, including all
197 Radeon pairs formerly absent. All **447** individually scoped display pairs
are named. Tests cover ATI Radeon/Rage, Matrox, Intel, 3dfx, S3, SiS, Trident,
Cirrus, NeoMagic and VIA, plus corrected C&T IDs and every explicitly scoped
PCI/USB pair. Compared with the previous tables, PCI gains 264 pairs and omits
144; USB gains 16 and omits none. PCI omissions follow the requested class
scope and Models-only extraction, including memory/APIC/timer functions and
registry rows previously mislabelled as `Software...SetupUpgradeDrivers`.
Mixed-class coverage is conservative; omissions are not assertions that the
underlying devices cannot exist in a retro PC.

Combined JSON is **79,305 bytes**, below `MAX_JSON = 409,600`. No compressed
encoding, loader change or increased limit is needed. The unchanged loader
allocates file size plus one NUL for each table, totalling **79,307 bytes** if
both are resident, excluding allocator and stdio overhead. This is an allocation
calculation, not a full-process memory measurement.

The reproducibility audit regenerated both tables in memory from the real
collection and pinned snapshots, compared their complete serialized bytes with
the tracked assets, checked every selected INF name retained precedence, verified
404 Display pairs, and checked every label's ASCII/length/escaping constraints.
Synthetic tests cover snapshot rejection before writes and oversize rejection
without replacing existing outputs.

## Host verification

Executed sequentially, each with `nice -n 19`:

| Command | Result |
| --- | --- |
| `python3 -m unittest tests.host.test_hwids -v` | 13 passed |
| `python3 -m unittest tests.host.test_devices -v` | 3 passed; existing ASan/UBSan settings preserved |
| `python3 -m unittest tests.host.test_credits -v` | 8 passed |

The Device Manager suite checked its existing 29 frames. No visual goldens were
updated. Session logs and exact commands are in `build/codex/f2-123-report.md`.
`git diff --check` passed. No new panic path exists; no kernel source changed.

## Lead review items

1. Correct the C&T examples in `video-driver-families.md`: pci.ids and the
   upstream [Linux PCI header](https://git.ti.com/cgit/wl12xx/wl12xx/tree/include/linux/pci_ids.h?h=master-2010-02-10&id=f5cd7872768d5856b1b409a33f516e5ac7798f75)
   identify 65554/65555 as `102c:00e4/00e5`, not `00c4/00c5`; `00c0` is 69000,
   while 65550 is `00e0`. The output uses the sourced IDs; no false aliases were
   invented. The binding contract is left for the lead to amend.
2. Review the bounded non-display scope and mixed System-class filtering. The
   public databases cannot establish exhaustive per-device class membership.
   More peripherals can be added as individually sourced pairs; broad claims
   of every historical/common device need a specified census and class source.
3. Integration/image evidence remains the lead's responsibility. No runtime,
   hardware qualification or complete Control Panel memory-use claim is made.
