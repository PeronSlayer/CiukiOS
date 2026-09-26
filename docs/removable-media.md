# Removable files in CiukiOS

`MEDIA.COM` reads external files without replacing the installed filesystem or
hooking DOS interrupts. The desktop's Floppy, USB and CD icons open this browser.
The same commands work from the DOS shell:

```
MEDIA FLOPPY
MEDIA USB
MEDIA CD
```

Inside the browser, use `DIR`, `CD folder`, `CD ..`, `TYPE file`,
`COPY file C:\destination` and `EXIT`. Escape returns to CiukiOS. Quotes are
accepted around names containing spaces. A command can also be supplied directly:

```
MEDIA CD DIR /DOCUMENTS
MEDIA FLOPPY TYPE /README.TXT
MEDIA USB COPY /PICTURE.BMP C:\PICTURE.BMP
```

`COPY` copies the complete file into the system filesystem, where the normal
DOS/Windows application can open it. Existing destination files are never
overwritten. A failed copy removes the incomplete destination. The external
source is always read-only.

## Device and filesystem support

| Medium | Access provided | Limits |
| --- | --- | --- |
| Floppy | BIOS drive 00, FAT12/16/32 directory traversal and file reads | Firmware must expose the drive; no USB-floppy hotplug driver |
| USB storage | FAT disks exposed through BIOS INT 13h before boot | Connect before boot and enable firmware USB legacy storage; no native UHCI/OHCI/EHCI/xHCI hotplug stack |
| CD | BIOS optical reads or legacy primary/secondary IDE ATAPI, ISO 9660 | No audio-CD file view, UDF, Joliet/Rock Ridge names, multi-extent files, native AHCI or USB optical driver |

FAT volumes can be unpartitioned or the first primary FAT partition in an MBR.
The parser accepts FAT12, FAT16 and FAT32, checks volume/partition boundaries,
uses the cluster count to select the format, supports fragmented chains and
handles FAT12 entries that cross sector boundaries. FAT filenames currently use
their DOS short names. GPT and extended/logical partitions are not mounted.

`MEDIA USB` selects the first readable BIOS FAT disk other than a CiukiOS system
volume. The BIOS may expose a second internal disk in the same namespace: the
browser reports the selected BIOS number and does not infer that every such disk
is physically USB. `MEDIA USB:81` selects BIOS disk 81 explicitly (80 through 87).
`MEDIA CD:IDE` bypasses BIOS optical access and selects the legacy IDE transport.

These are browser/read/copy operations. The module does not create DOS drive
letters or make every external file accessible directly to arbitrary programs.
This distinction avoids presenting an icon as a filesystem driver.

## Validation

Build using `bash scripts/build_media.sh build/full/media-work`. The toolchain
is the installed `ia16-elf-gcc`, NASM and IA16 binutils. The executable has its own
4 KiB stack and restores the full register and segment state around firmware
calls, including BIOS AH=08's ES:DI result. Every extended read rebuilds the disk
address packet and transfer count. Reads have bounded retries; ATAPI does not
reset the channel or write source media.
BIOS sector transfers use a private buffer aligned to a physical 2 KiB boundary,
so neither 512-byte floppy DMA nor 2048-byte CD reads cross a physical 64 KiB
boundary when DOS loads the COM at a different segment.

`uv run --with unicorn==2.1.4 python scripts/test_media_readers.py` runs the
assembled production COM, not a separate parser reimplementation. Disk fixtures
are created with `mkfs.fat`/`mtools`/`xorriso`; fragmented payloads are checked with
the independent `mtype` reader. Cases cover all four filesystems, nested paths,
exact text and binary content, deliberately clobbered BIOS registers/segments,
modified DAP counts, failed I/O, invalid geometry, broken/cyclic chains, partial
copy cleanup, MBR offsets and refusal to overwrite.

`python3 scripts/qemu_test_media.py` boots an actual installed HDD image and
attaches a floppy, a USB mass-storage device and an IDE CD. It traverses nested
directories and copies all payload bytes through each device backend, then reads
the destination files independently with mtools after QEMU exits. Hashes also
prove the external fixtures were not modified. This establishes behavior on
QEMU/SeaBIOS devices, not verification of the user's physical firmware or T23.

## Sources

The on-disk layouts follow Microsoft's [FAT specification 1.03](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
and [ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/).
The BIOS USB test uses SeaBIOS's actual [USB mass-storage implementation](https://github.com/coreboot/seabios/blob/master/src/hw/usb-msc.c).
No third-party driver code was copied into this module.
