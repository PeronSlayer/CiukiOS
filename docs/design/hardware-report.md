# Hardware Report (f9-07)

The application is a read-only diagnostic client. Export is an explicit action
after a section summary and a privacy notice; it never sends data over a network.
The destination contains `CIUKIREP.TXT`. No user directory is scanned.

## Sources and decisions

- [Intel SDM, CPUID](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  use leaf 1 ECX/EDX feature bits, never processor serial-number leaf 3. Existing
  call 76 already supplies the decoded vendor/family/model/stepping.
- [DMTF SMBIOS 2.5, sections 3.1, 3.3.1 and 3.3.2](https://www.dmtf.org/sites/default/files/standards/documents/DSP0134v2.5Final.pdf):
  reuse the checked boot-time SMBIOS parser and its bounded string copies. Add
  Type 0 offset 8 (BIOS date) only when the formatted record contains it. Export
  manufacturer, product and BIOS identity, never serial numbers or UUIDs.
- [ACPI E820 system address map](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html):
  summarize retained, validated boot ranges by type, separately retaining usable,
  reserved, reclaimable, NVS and bad memory. Unknown types have their own bucket.
  No memory limits or reservations change. The UEFI HTML server returned HTTP 403
  during research; its indexed specification text confirms these type meanings.
- [UEFI PI 1.8 IDE Controller](https://uefi.org/specs/PI/1.8/V5_IDE_Controller.html)
  and [National Semiconductor PC87415 datasheet, register 09h](https://parisc.docs.kernel.org/en/latest/_downloads/f2e77904a62f96d8728841bbf1197b5d/PC87415.pdf):
  decode PCI IDE programming-interface bits 0/2 as the primary/secondary native
  versus compatibility mode, and bit 7 as bus-master capability. Capability
  does not establish active DMA; retained driver bindings report the active path.
- Windows 98 Resource Kit, Chapter 27, local `wrkc27_3.htm`, “Microsoft System
  Information”: expose installed devices, driver status and hardware resources
  together. A shared PCI IRQ alone does not establish a conflict.
- [RFC 1952](https://www.rfc-editor.org/rfc/rfc1952) defines the optional gzip
  archive. Reuse the existing [stb_image_write DEFLATE compressor](https://github.com/nothings/stb/blob/master/stb_image_write.h)
  with static linkage, replacing its zlib framing with gzip framing, CRC32 and
  ISIZE. Gzip headers carry no timestamp, filename or machine metadata. No new
  third-party code is vendored; the existing stb notices remain in the image.

## Additive call 76 contract

Version 3 with exact `sizeof(struct ciuki_hardware_info)` returns a zero-filled
copied record: map source/count, leaf 1 flags, SMBIOS validity and bounded identity
strings, totals for memory types 0 (unknown), 1 through 5, and kernel build ID.
The same record includes the bounded 4 KiB pending log snapshot, loss count,
sink status and the ring write position/start/limit. The exact record is 4500
bytes, aligned to four bytes; every field has a literal ABI oracle entry.
This operation uses retained boot state and CPUID, with no firmware evaluation, device probing,
allocation or physical pointers. Versions 1 and 2 are unchanged. A missing SMBIOS
cache is represented by validity zero and empty strings.

## Privacy, bounds and errors

Inventory export names devices and drivers, resources, ATA programming-interface
bits, display modes, firmware hierarchy and capabilities. Network output uses
only the interface driver/type, binding and carrier state, never MAC/IP/DNS,
host names, DHCP configuration or registry identity. Boot options, process lists,
volume labels/identifiers and SMBIOS serial/UUID fields are excluded. Live FAT
quarantine reasons can embed user filenames, so those reasons are withheld too.

Logs are not safe to copy verbatim. The export keeps known hardware event tags
and a small allowlist of numeric diagnostic fields; all other payload is replaced
with a redaction marker. The boot prefix and recent disk records are read with
fixed memory bounds. BOOT.LOG wraps inside a fixed-size file: physical EOF is
not necessarily its newest point. The retained write position selects the
recent window, joining the end and beginning of a wrapped ring in time order.
A separate section contains the newest pending RAM slices (oldest first),
including logs captured before a disk sink exists. Querying neither consumes
records nor changes the producer, watermarks or reservations; it adds no log
retention buffer. A racing disk drain may overlap the two snapshots; no atomic
whole-system snapshot is claimed. Optional `CIUKILOG.GZ` contains the full privacy-filtered BOOT.LOG,
BOOT1.LOG and BOOT2.LOG (missing rotated files are identified). The user can
prepare, preview and exclude this archive independently. The report and preview buffers have a 512 KiB limit. Full sanitized logs
have a 4 MiB bound, previewed in successive 512 KiB parts without enlarging
the toolkit text/undo/layout allocations. Archive preparation refuses overflow
instead of silently truncating. Source scans have a 16 MiB/65536-fragment per-file budget. Rotated archives
retain physical file order; only the report uses the current boot write position.
The archive uses the same privacy filter; it never contains raw log payloads.

Export writes a temporary file on the selected medium, checks every write, fsync
and close, then renames to the final short name. Failed exports leave any previous
report intact and report the error. USB FAT volumes are preferred, then a writable
floppy root, then the system drive. A removed medium must never silently fall back
to a different destination. Rescue callers must restrict destinations to USB.
Candidate drives are found without test writes; mode bits and the media ABI are
advisory and the checked export is the final writability test. USB generation is
checked before opening and before publication; cleanup also refuses a changed
generation. The report and optional archive are separate atomic file publications;
if the archive fails after the report succeeds, the UI says exactly that.

## Integration dependencies

This source state has no Setup application or rescue page. Its integration should
supply a granted desktop channel when spawning
`/Programs/hwreport --channel-fd=3 --rescue`; this requires the future Setup
launch wiring. The rescue argument restricts export to a currently present
USB FAT drive. The ordinary Programs menu launches the app without this flag.

The canonical builder writes `build-manifest.json` beside the host image. It does
not publish it in the guest. An exact hash of an image cannot be embedded in that
same image without changing the hash. Until the release/Setup pipeline provides
trusted external image provenance, the report explicitly marks image SHA-256 and
build manifest unavailable and includes the kernel build ID. No invented hash or
unqualified manifest is substituted. Lead review is required for this handoff.
