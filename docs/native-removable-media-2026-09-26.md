# Native removable media

CD-ROM, USB drive and Floppy desktop icons now open the native **Files** window.
No command interpreter or text video mode is involved in these actions.

- **Open** enters a folder or displays a bounded, read-only ASCII text preview.
  The preview reads at most 511 bytes and the status explicitly identifies it as
  a preview. **Refresh** returns to the containing directory.
- **Copy** asks for an absolute destination filename on the system disk and
  imports the entire file, including binary data. Existing files are preserved;
  an unsuccessful import removes its partial destination.
- **Up**, **Previous**, **Next** and **Refresh** navigate native directory lists.
  Source media are read-only. Rename, Move, New folder and Delete explain this
  restriction inside Files rather than launching DOS.
- Missing media, unreadable sectors, unsupported filesystems, binary previews
  and existing destinations produce native status messages. Reopen **Files**
  from its desktop icon to return to the ordinary local system drive.

The USB icon uses additional disks exposed through BIOS INT13h. Such a disk can
also be an additional internal drive: firmware does not provide a reliable
USB classification through this interface. CiukiOS volumes are skipped when
selecting a disk. Connect the USB device before boot and enable legacy storage
in firmware. This is **not** a native UHCI/OHCI/EHCI host-controller stack and
provides no USB hot-plug guarantee. The GUI identifies it as a read-only BIOS
disk. CD access supports BIOS optical disks and legacy IDE ATAPI; SATA/AHCI and
USB optical controllers still require appropriate drivers.

FAT12/16/32 use DOS short names. CDs use the primary ISO9660 names. GUI labels may
elide names longer than 12 characters, while open/import use the original name
retained in the media service. Execution directly from a removable filesystem
is not supplied by this read-only browser: import an executable and its required
data first. The service does not claim to mount these filesystems into the DOS
kernel's ordinary drive namespace.

## Implementation

`src/com/media.c` is built both as the unchanged command-line `MEDIA.COM` and as
`SYSTEM/MEDIA.DRV`. The module is called directly in foreground, with a private
stack and no EXEC, keyboard wait or console/video output. Sector parsing, bounds
checks, fragmented-chain walking, BIOS retry handling and safe import are shared.
Painting only consumes previously cached data.

Build the module with:

```sh
bash scripts/build_media_driver.sh build/full/obj
```

The raw module header contains an entry jump, `CMEDIA01`, ABI version 1, image
length, allocation paragraphs, the 1192-byte request size and an exported request
offset. `src/com/media_driver_abi.h` defines the packed request and operations.
The module preserves all general and segment registers and incoming flags across
the FAR call. It accepts an external ES:DI request or its own exported request.
The shell validates the header and shrinks the allocation after reading the
image; the module remains cached while that shell instance is alive.

## Evidence

`build/full/native-media-2026-09-26/qemu-r3/report.json` records real QEMU input
against a private image, Pentium III CPU model, 128MiB RAM, native 800x600 desktop.
The test used rendered mouse controls and PS/2 keys; guest memory was observed,
not modified. Floppy, BIOS USB and CD each passed nested navigation, native
preview, full binary import, refusal to overwrite, binary-preview warnings and
read-only source warnings. Missing floppy and returning to local Files passed.
The three 8965-byte imports were checked independently after QEMU exit and had
SHA256 `ab0bdb9c08bc2d98466053fb18a8b9c96b159f6e6d4ffeaa33163b61b9ca7834`.
All read-only source fixture hashes remained unchanged. Screenshots are in that
same directory.

`build/full/native-media-2026-09-26/driver-tests-r2/report.json` executes the
actual compiled FAR module with modeled BIOS/DOS calls. It covers FAT12, FAT16,
FAT32 and ISO9660, hostile firmware segment/register changes, DMA boundaries,
import bytes, name selection, pagination, removal, errors and ABI preservation.
This supplements the QEMU device tests and does not replace physical testing.

The command-line variant's regression report is
`build/full/native-media-2026-09-26/cli-regression/tests/result.json`.
Earlier `qemu` and `qemu-r2` directories retain failed candidates: the first
exposed a harness resolution assumption; the second found and led to the fix
for stale double-click history across newly entered folders. They are not
acceptance evidence. None of these checks qualifies a physical T23 or E500.
