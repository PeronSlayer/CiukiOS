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

## Build identity (f9-07 amendment 1)

The `[Build identity]` section quotes `/System/BUILD.TXT`, which
`scripts/build_identity.py` stages into every image (`schema=1`,
`source_commit`, `source_dirty`, `kernel_elf_sha256`, `build_manifest_id`,
`build_date`, `image_sha256=see release page`). The application reads it
read-only through the ordinary file API, at most `HR_BUILD_MAX` (1024) bytes;
one extra byte is read to prove an oversized file, which is never truncated and
quoted. Every quoted value is checked against its exact shape first (commit:
`unknown` or 40 lowercase hex digits; kernel SHA-256 and manifest id: 64 hex
digits; date: `YYYY-MM-DDTHH:MM:SSZ`; schema 1; no control or non-ASCII bytes;
no duplicate known keys; all required keys present). A missing file, an
oversized file, a read error and a malformed file each get their own plain
statement and quote nothing; a file that was not read says so. Nothing is
invented. The image SHA-256 stays out of the report: an image cannot contain
its own hash, so the report says to quote it from the release page. Host tests:
present (with CRLF, no-git and no-trailing-newline forms), missing, oversized,
read error, twelve malformed shapes, and the bounded native read.

Since fx-6 `kernel_elf_sha256` is the SHA-256 of the installed
`/system/VMM.ELF` (the kernel without debug sections, the file on the disk);
BUILD.TXT also carries `kernel_full_elf_sha256`, the complete
`build/f0/VMM.ELF` that `addr2line` needs. The report ignores keys it does not
know, so it quotes the installed kernel only; both lines fit `HR_BUILD_MAX`.

## Setup rescue page (f9-07 amendment 1, revised by the lead on 2026-10-23)

In Setup mode there is no desktop: the supervisor starts `/Programs/setup`
instead (`src/kernel/proc/supervisor.c`, `setup_bootstrap`), and Setup serves no
window protocol. The rescue page therefore runs the program **headless** and
Setup is the viewer and the writer.

**Program side (`hwreport --rescue --text-fd=N`).** No window, no drive
selection and no file write. It builds the same `CIUKIREP.TXT` text as the
interactive export (`hr_format`: same sections, same privacy filter, the
`/System/BUILD.TXT` provenance) and sends it over the channel on descriptor N
(3..63; any other argument, or `--text-fd` without `--rescue`, is exit 2) with
`CIUKI_SYS_CHANNEL_SEND`, blocking, then exits 0. Differences from the
interactive export, both inherent to having no desktop: there is no 2D-engine
record (that query goes through the desktop, so the report says "2D engine
query unavailable; see bindings"), and the optional full-log archive is not
produced. `hr_link_report` (`apps/hwreport/textlink.c`) is the testable core.

**Frames (`apps/hwreport/textframe.h`, shared with Setup).** One frame is the
data of one channel message (at most 256 bytes): `u32` magic `CHRL`, `u16`
version 1, `u16` type, then BEGIN (total length), DATA (offset, 1 to 244 text
bytes) or END (`i32` status, total length, CRC-32 of the text). A transfer is
BEGIN, DATA*, END and nothing after END. Hard bound: **240 KiB (`HR_LINK_MAX`)**,
16 KiB under the 256 KiB of Setup's text viewer (`ST_TEXT_BYTES`, asserted at
compile time); a longer report is cut at a line boundary and sent with status
`-EFBIG`. A collection error sends no text and that error as the status.

**Setup side (`apps/setup/report_link.c`, native operations in
`apps/setup/native.c`).** `ciuki_channel_pair`, `fcntl(child_end, F_SETFD, 0)`
(the spawn mapping rejects CLOEXEC sources), `ciuki_spawn("/Programs/hwreport",
{"hwreport","--rescue","--text-fd=3"}, env, {child_end,3}, 1, 0)`: the recipe of
the desktop's `spawn_demo`. Receive is `poll` on the channel (slices of at most
1 s, no busy polling) plus `ciuki_channel_recv(DONTWAIT)` under a 15 s deadline
from the spawn; each frame goes through the validating state machine. After END
the child is reaped within 2 s (`waitpid(WNOHANG)` at 10 ms sleeps) and must
have exited 0 with END status 0. On every failure path (timeout, closed, bad
frame, oversized, bad checksum, bad status or exit) both channel ends are closed
and a running child is killed (`ciuki_proc_terminate(SIGKILL, -1)`: descendants
need no authority) and reaped within 3 s. Reasons logged: `memory`, `pair`,
`missing` (spawn `-ENOENT`), `spawn`, `timeout`, `closed`, `frame`, `size`,
`checksum`, `status`, `exit`, `empty`, `unavailable`, `failed`.

**Result.** Success: the text opens in Setup's text viewer (back to the rescue
page), `[rescue] action=hardware_report result=app bytes=<n>`. Any failure:
`[rescue] action=hardware_report result=fallback reason=<r>` and Setup's own
report, and a text received earlier is dropped so a stale or partial report is
never saved. "Save report to USB" writes `HARDWARE.TXT` (Setup's own report:
verdict, disks, partitions, which an installer support case needs) and, when
the f9-07 text was received, `CIUKIREP.TXT` next to it, the file name of the
f9-07 contract; the two are kept side by side because they answer different
questions and replacing `HARDWARE.TXT` would drop the disk inventory. The
Welcome and refusal pages keep showing Setup's own report. The Setup CD payload
carries `/Programs/hwreport` next to `/Programs/setup` (`scripts/build_cd.py`,
`SETUP_PROGRAMS` and `check_setup_programs`, which also require both to be ELF
files).

**Authority in Setup mode.** `setup_capability` is never inherited
(`supervisor.c`, `supervisor_dosvm_delegate`), so the spawned `hwreport` runs without it.
Every inventory call it makes was checked in the kernel: `CIUKI_SYS_DEVICES`
for the device list, the hardware record (call 76 version 3) and the firmware
pages (`devices_query`: only pointer, version and size checks), display query
(public), `/dev/net` `NET_GET_STATE` (`net_devctl`: the capability gate applies
only to `CIUKI_SETUP_NET_QUERY`, which hwreport never calls) and `/dev/media`
`MEDIA_GET_STATE`/`MEDIA_GET_VOLUME` (no authority check), plus read-only file
opens. None refuses without the capability. What Setup mode does change is
the data: the boot log is not activated in Setup mode (storage is read-only),
so `/system/logs/BOOT.LOG` can be absent and the log sections then read
"Unavailable (error N)" or "No pending records", and ACPI/firmware paging may
report "Unavailable/incomplete (error N)" if the platform is not initialized.
Those sections say so; they never fail the whole report. Qualification on the
Setup CD itself is by the lead's later runs (not run here).
