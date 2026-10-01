# CiukiOS driver assets

The installed catalog lives at `\DRIVERS\CATALOG.TXT`. Native keyboard, PS/2
mouse, BIOS disk and VBE drivers are part of CiukiOS. Their presence does not
depend on copying a Windows driver into this directory.

`HWDETECT.COM` selects a profile from actual PCI IDs and firmware capabilities;
its interface is documented in `docs/driver-catalog.md`. Unknown hardware keeps
the generic paths and is reported explicitly. Existing `\SYSTEM\DRIVERS` and
`\SBEMU` paths remain compatibility aliases for existing launchers.

## CuteMouse

The optional DOS mouse driver is CuteMouse 2.1b4, by Nagy Daniel, Eric Auer and
contributors, using Davide Bresolin's published Linux/JWasm port. It remains a
separate GPL program. CiukiOS does not incorporate its source into the kernel.

- Original project: <https://cutemouse.sourceforge.net/>
- Official FreeDOS 1.4 package:
  <https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/base/ctmouse.zip>
- Port: <https://github.com/davidebreso/ctmouse>
- Pinned commit: `0dd8f324bd465376ec3778c785f33398bf909619`.
- `ctmouse/CTMOUSE.EXE`: rebuilt from that commit with JWasm 2.21 and GCC.
- `ctmouse/CTMSRC.ZIP`: the complete corresponding source, including makefile,
  GPL license, upstream notes and the MIT-licensed `bin2exe.c` by Raphaël
  Assenat and Davide Bresolin.
- `ctmouse/COPYING`: GPL version 2. `ctmouse/bin2exe.c` also retains its own
  copyright and permission notice.
- Reproduction: `bash scripts/build_ctmouse_driver.sh`.

The upstream FreeDOS archive, original binary (`CTM-FDOS.EXE`) and original
`SOURCES.ZIP` are retained here for provenance. The runtime package uses the
rebuilt `CTMOUSE.EXE` and its matching `CTMSRC.ZIP`; do not substitute the
FreeDOS source archive for the source corresponding to the ported binary.

CuteMouse is an **optional replacement**, not an automatic second mouse
driver. `/P /W` selects PS/2 and conventional memory. `/B` refuses to load over
an existing INT 33h driver; without `/B` the native owner must first release
the mouse. See the upstream manual before manual use.

## Era driver pack

The packager verifies each installed file against `payloads.json`. That file
records the original archive URL, its SHA-256, each extracted file's SHA-256,
and its license. The complete matching source archives are installed with the
drivers. `DRIVERS.CFG` uses `@PCI` to compare `Hardware=` in each `DRIVER.INF`
against PCI IDs; `%PKT%`, `%IRQ%`, and `%IO%` come from the matched device.
Missing matches are skipped. `LOADDRV.LOG` records load, skip, and error
results. The loader's 20-second watchdog covers drivers that leave interrupts
enabled; a driver that disables interrupts and hangs cannot be recovered.

| Hardware | Driver | License | Startup rule | Upstream |
|---|---|---|---|---|
| NE2000 PCI / RTL8029 | Crynwr NE2000 | GPL v1 | PCI match | [FreeDOS Crynwr](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/crynwr.zip) |
| AMD PCnet PCI | Crynwr NE2100 | GPL v1 | PCI match | [FreeDOS Crynwr](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/crynwr.zip) |
| Realtek RTL8139 | RTLPKT with a documented one-byte PCI ID adaptation | GPL v1 | PCI match | [SourceForge RTLPKT](https://sourceforge.net/projects/rtl8101packetdr/files/) |
| Intel 82540EM PRO/1000 | E1000PKT | GPL v2 | Disabled; QEMU reports no PCI BIOS to this driver | [FreeDOS E1000PKT](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/e1000pkt.zip) |
| Intel PRO/100 | E100PKT | GPL v3 | Disabled; QEMU eepro100 transmit unverified | [FreeDOS E100PKT](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/e100pkt.zip) |
| 3Com 3C509 ISA | Crynwr 3C509 | GPL v1 | Disabled; manual ISA configuration | [FreeDOS Crynwr](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/crynwr.zip) |
| APM BIOS | FDAPM | GPL v2 | Disabled; opt in after hardware check | [FreeDOS FDAPM](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/base/fdapm.zip) |

The existing BIOS disk and VBE paths and CiukiOS input/audio modules cover
the generic baseline. SBEMU/VSBHDA remain the existing application-launch
audio route for supported PCI hardware. CD-ROM redirectors and USB storage
have no validated resident loading path yet, so this pack does not claim
automatic CD or USB support. Likewise, hardware families beyond the IDs in
the installed INF files still need a licensed driver and a safe matching rule.
The integration evidence is QEMU only; physical PCs remain untested.

## Manufacturer packages

Original HP/Compaq SoftPaqs downloaded for the E500 investigation are under
`build/full/e500-driver-research-2026-09-26`, outside the redistributable assets.
They are Windows packages and no general redistribution permission or native
CiukiOS driver compatibility has been established. Details and primary-source
links are in `docs/e500-drivers-2026-09-26.md`.
