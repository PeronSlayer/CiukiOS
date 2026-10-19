# FreeDOS 1.3 reference packages (conformance battery f3-48/f3-51)

Official FreeDOS 1.3 release material, used only as the conformance reference
guest of the DOS battery (`tests/suites/f3-dos-conformance.json`); CiukiDOS
stays the DOS of CiukiOS (dev diary 2026-10-19-06). Nothing here is compiled
from master sources.

| Role | Archive (not tracked; re-download) | Member | Version | SHA-256 of the member |
| --- | --- | --- | --- | --- |
| kernel | `kernel.zip` from `repositories/1.3/base/` | `BIN/KERNL386.SYS` | 2043 | `932c0c155701eddb7b902f7269a1b2ce31f5c82a6dc195172f2336d18a74e1fb` |
| shell | `freecom.zip` from `repositories/1.3/base/` | `BIN/COMMAND.COM` | 0.85a | `93ed257d3aa2454821aa305b731544550dcccff898865b590deb972c47b48610` |
| share | `share.zip` from `repositories/1.3/base/` | `BIN/SHARE.COM` | 08/2006a | `b51a1af2188d41096e7983b2eef7a7bff8ef9fccffbbb3db93d4e49eb10b6e55` |
| boot | `FD13-FloppyEdition.zip` from `distributions/1.3/official/` | `144m/x86BOOT.img`, sector 0 | 1.3 | see `UPSTREAM.json` |

`UPSTREAM.json` pins every archive, member and licence text by SHA-256;
`scripts/dos_conformance.py` refuses anything that does not match. The zip
archives are ignored by git: stage them from the URLs in `UPSTREAM.json`
(`https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/`), verify the
`package_sha256` values, and keep them in this directory. Licences: GNU GPL
(kernel `DOC/KERNEL/COPYING`, FreeCOM `BIN/FREECOM/LICENSE.TXT`, SHARE's
copying policy in `APPINFO/SHARE.LSM`); the reference image carries them as
`/D48/*.LIC`. Provisioned by the lead on 2026-10-19 from the ibiblio mirror.

Amendment 1 continuation: the floppy sector above is a **FAT12** loader. Its
executable code cannot boot the private FAT16 disk by changing BPB fields.
The installed reference uses the exact FAT16 template embedded in the same
official package's `BIN/SYS.COM`, without rebuilding a FreeDOS component.
`boot16.bin` is that 512-byte template (SHA-256
`39586cd0f55f7732791a990770163d4af9827623e418203921d6f23819b94cb5`).
`UPSTREAM.json::boot_fat16` records the package, compressed SYS member and
extraction stop/address. `tests/host/dos/reference_boot_test.c` reproduces
these bytes with the existing host CPU emulator, stopping at the unpacker's
first INT21, before SYS's installation logic or any DOS/block services run.
The original floppy pin is retained as provenance, not used as a FAT16 VBR.

The template is GPL-2.0-or-later, copyright Svante Frey (1997) and the FreeDOS
contributors. `COPYING` is the exact kernel package license. Corresponding
source is `SOURCE/KERNEL/SOURCES.ZIP::boot/boot.asm` in the pinned official
`kernel.zip` at the URL above; `sys/sys.c::put_boot` selects the FAT16 template.
The canonical builder changes only its OEM/BPB/volume-label fields, and wraps
it in a deterministic MBR with one partition at LBA 63. The kernel, shell and
SHARE binaries retain the original pinned hashes. Actual boot qualification
remains lead-owned.
