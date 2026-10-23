# f1-115 floppy controller implementation

Research checked 2026-10-23. Runtime and hardware qualification remain with
the lead; this record describes the host-testable implementation boundary.

## Sources and decisions

- [Intel 82077AA, 290166-007](https://datasheet4u.com/pdf-down/8/2/0/82077AA_IntelCorporation.pdf),
  sections 2, 4, 5 and 7: use explicit command/result phases, reset interrupt
  draining, seek/recalibrate completion and ST0–ST2. DMA2 uses a pinned
  below-16-MiB bounce page which cannot cross 64 KiB. Claim only DOR,
  MSR/FIFO and DIR/CCR, plus IRQ6 and DMA2; never ATA's 3F6h.
- [Linux v6.12 drivers/block/floppy.c](https://github.com/torvalds/linux/blob/v6.12/drivers/block/floppy.c),
  `floppy_type`, `perpendicular_mode`, `fdc_specify`, `fdc_dtr`,
  `disk_change`: geometry and rate are media properties; changing rate
  requires SPECIFY again. 2.88 MB requires perpendicular mode. Bound retries
  and report write protection without retrying a forbidden write.
- [SeaBIOS rel-1.16.3 src/hw/floppy.c](https://github.com/coreboot/seabios/blob/rel-1.16.3/src/hw/floppy.c),
  `floppy_enable_controller`, `floppy_pio`, `floppy_read`, `floppy_write`:
  drain four reset SENSE INTERRUPT replies, wait for motor spin-up, and
  validate result phase after IRQ. BIOS services are implemented independently;
  no BIOS or donor driver is executed by the native kernel.
- [Renesas/Intersil 82C37A, FN2967 rev. 4](https://www.renesas.com/en/document/dst/82c37a-datasheet),
  compatible register programming: mask channel 2, reset the shared byte
  flip-flop under a short interrupt exclusion, set address/count/page/mode,
  then unmask. Inspect channel 2's count rather than clearing other channels'
  terminal-count status bits. Guest DMA uses checked VM memory only.
- [ACPI 6.5 device identification](https://uefi.org/specs/ACPI/6.5/06_Device_Configuration.html):
  PNP0700 HID/CID resources are discovery facts, not permission to probe.
  Retain BDA/CMOS drive evidence; reject non-primary/unqualified routing.
- Owner-supplied Windows 98 Resource Kit, read-only
  `/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc28_5.htm:18–85` and
  `wrkc10_9.htm:140–146`: native block I/O remains below filesystem
  arbitration; do not infer FAT type or truncate BPB geometry from a floppy
  device name. No Microsoft text or code is imported.

Repository comparison: no native FDC exists. `blkdev.h` already supplies
media generations and cache-disabled durability; the physical floppy adapter
uses that interface. FAT12 parsing remains f1-115b; the mount adapter here
publishes that driver's whole-media view at A:/B:.
`dosvm_dma.c` already owns guest 8237 state; the virtual FDC consumes that
state without programming physical DMA. The firmware inventory is asynchronous
and read-only; its cached PNP0700 records are consumed after completion.

No donor implementation is copied or vendored. Network downloads from the
sandbox are unavailable; sources above were inspected through the web tool.
Any downloaded donor files must remain under `build/research/`.

## Private interface

`src/kernel/drivers/floppy.h` is kernel-private, with no syscall or public SDK
change. A physical drive has exactly one lease: native filesystem/raw client,
or a VM identified by its immutable ID/generation. The native block view is
usable only while its lease is held. Release waits for the controller gate;
old lease copies cannot release or access a later owner. Media changes advance
the block generation and require explicit revalidation before I/O resumes.
The FAT12 mount must unmount/invalidate its view before releasing the lease.

Disk-image assignment opens the image with VFS deny-all sharing, retains the
file description until VM detach and requires an exact supported media size.
Native files and other VMs cannot open it concurrently. Port reset is virtual;
it never resets the physical controller or revokes another drive's owner.

A physical VM lease can exist with blank or absent media. Unformatted media
cannot pass READ ID, so explicit FORMAT instead acknowledges DIR with a real
step and checks positioning before issuing the format DMA. A still-asserted
DIR or write protection refuses the operation. The format path never treats
a successful READ ID as a prerequisite for creating sector IDs.

INT13 uses the floppy BDA status byte at 441h independently of hard-disk status.
Its diskette parameter tables occupy the spare bytes at F000:01E3–01ED and
01F3–01FD, after the existing three-byte BIOS stubs. No new ROM page or DFVM,
DPMI or public ABI reservation is used. Assignment publishes the INT1E pointer
to the first assigned drive's table. IRQ6 and DMA2 status belong to the VM;
DMA count, direction, page boundary and guest buffer permissions are checked
before a provider transfer. CRC and missing-mark status are retained through
the virtual result phase and mapped to distinct BIOS errors.
