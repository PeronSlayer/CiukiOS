# Peripheral integration research and decisions

This integration follows `docs/directives/integ-periph-peripheral-integration.md`.
The individual driver design/validation records and retained donor notices
remain the source for controller-specific algorithms and admission limits.

Before integration, the shared code was compared with these primary sources:

| Source | Comparison and integration decision |
| --- | --- |
| [SeaBIOS rel-1.16.3 `src/serial.c`](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/serial.c), `serial_setup`, `lpt_setup` | COM count occupies equipment bits 9–11 and LPT count bits 14–15. Replace the existing INT 11h literal with the VM's BDA word; preserve each frontend's count update and unrelated equipment bits. |
| [USB-IF HID 1.11](https://www.usb.org/sites/default/files/documents/hid1_11.pdf), sections 6.2.2.7 and 8 | A report ID consumes a wire byte. Retain f1-104b's 64-byte bound and DMA-slot guards alongside f1-106b's split scheduling and persistent interrupt transfers. |
| [Intel EHCI 1.0](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf), sections 3.6 and 4.12 | TT routing/scheduling fields and QH retirement are separate from payload capacity. Combine both changes without changing the branch's scheduling or stop rules. |
| [Microsoft FAT specification 1.03](https://www.fysnet.net/docs/fatgen103.pdf), FAT type determination and FAT12 entries | FAT type follows cluster count; the same FAT driver serves the floppy block adapter. Preserve packed-entry handling and f1-115b's DMF/write-protection policy under f1-115's A:/B: mount/lease path. |
| [Intel/Microsoft APM 1.2](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF), real-mode connection and power-state/event services | Preserve Amendment 1's contained BIOS-VM calls and exclusive provider interface. Missing global platform services continue to refuse connection. ACPI events/UI integration remains a later task. |
| [Linux v6.12 `sound/pci/es1968.c`](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/es1968.c) and [`sound/pci/emu10k1/emu10k1_main.c`](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/emu10k1/emu10k1_main.c) | Keep the branch's actual chip distinctions instead of collapsing Maestro generations or Live!/Audigy into one discriminator. Assign unique sequential Ciuki transport IDs after the unchanged values 1–6. These numeric IDs are Ciuki policy, not donor constants. |

The owner's read-only Windows 98 Resource Kit was consulted at
`/home/peronslayer/Desktop/CiukiOS/build/rskt`: `wrkappb_1.htm` (DMF),
`wrkc24_3.htm` (VCOMM arbitration), and `wrkc30_3.htm` (HID mapping).
Keep native hardware ownership with leased virtual DOS frontends and the
existing input mapping. No Resource Kit text or code is imported.

Fresh retrieval of SeaBIOS `src/misc.c`/`src/std/bda.h` and Linux's Trident
source failed at the web cache; no contents from those failed reads are
claimed. The successful serial source establishes the equipment bit fields.
No new donor implementation is introduced by the integration itself.

Integration measurements: all enabled virtual peripherals require 23 disjoint
port-handler ranges. Increase the private descriptor table from 16 to 24;
canonical i686 layout measures 192 to 288 descriptor bytes, while the complete
page-aligned `struct dosvm` stays 786432 bytes at the base and integrated state.
No VM allocation, memory quota or global resource capacity changes. This avoids
creation failures without combining unrelated port ranges or allowing holes.
Game-port inventory records use 0x72000 onward; LPT retains 0x71000 onward,
so both can coexist without duplicate Device Manager identifiers.

The fixed-disk-only storage service loops previously started at C:. With
native A:/B: mounts, cache refresh, delayed-error publication, shutdown
preflight/commit/detach and bounded power flush must visit all mounted FAT
letters. Extend those loops to A: and exercise the power barrier at A:, B:
and D:. Boot-time fixed-disk discovery and optical letter assignment retain
their existing starting letters.

The Control Panel render fixture now enumerates the added Game controllers
page. Only `control-category-2` (the additional Input applet) and the new
`control-joystick` golden change; both rendered PNGs were visually inspected.
All other existing frame hashes are retained.
