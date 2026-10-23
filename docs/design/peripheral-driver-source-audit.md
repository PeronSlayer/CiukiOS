# Peripheral driver source and license audit

This appendix is part of [peripheral-driver-coverage.md](peripheral-driver-coverage.md)
and records the f1-102 candidate-source boundary. Research snapshot: 2026-10-22.
No source is imported into the kernel by this change. All downloaded research
files are ignored under `build/research/f1-102/`.

## Method and permission boundary

Each listed source file was fetched from its upstream repository at the stated
ref through the GitHub read API. Its license header was read; legacy grants
were checked through their complete alternative-license paragraph. Git blob
SHA-1 below identifies upstream bytes, not a compiled artifact or image.
Line counts include comments and blank lines and are only source-size evidence.

The Linux manifest is a bounded set of algorithm/constant sources to adapt into
Ciuki's existing interfaces. It is not a proposal to compile these files with
all their Linux includes or port ALSA, libata, TTY, input or driver-core wholesale.
The ACPICA manifest is a prospective runtime source/header set with a new Ciuki
OS Services Layer; compiler/debugger/disassembler, foreign OS glue, firmware and
build tools are outside it. Any additional copied dependency or changed version
needs its own header audit in the implementing directive. Preserve upstream
copyrights and grants alongside adapted portions, modification notices and the
kernel's GPL v2 source-delivery obligations. This is a file-specific engineering
license verdict; the name of an operating system is not its license.

## Linux v6.12

Every link below is under `torvalds/linux`, tag **v6.12**. Header spellings are
retained verbatim in the license column. Verdict **G2** means compatible with
Ciuki's GPL-2.0-only kernel by selecting GPL version 2:

- `GPL-2.0` is the older SPDX spelling for GPL-2.0-only.
- `GPL-2.0+` / `GPL-2.0-or-later` allow choosing version 2.
- `GPL-1.0+` also allows choosing version 2: `ohci-pci.c` and the underlying
  grant in UAPI `serial_reg.h` are these exceptions, not GPL-2.0-only headers.
- `WITH Linux-syscall-note` on UAPI headers is an additional userspace inclusion
  exception. Preserve it if taking these constants; it does not place Linux
  implementation code or Ciuki's entire public ABI under MIT.
- `pata_ali.c` has **no SPDX**: its header allows GPL copying/modification and
  `MODULE_LICENSE("GPL")` agrees. Select GPL v2 in the Linux distribution's
  [COPYING context](https://github.com/torvalds/linux/blob/v6.12/COPYING), retain
  that legacy grant, and document any adapted portion. Do not invent a BSD grant.

| Exact candidate path | Header license / verdict | Text lines | Git blob SHA-1 |
| --- | --- | ---: | --- |
| [`arch/x86/kernel/apm_32.c`](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/apm_32.c) | GPL-2.0-or-later; G2 | 2410 | `b37ab10957074fb60211db3ec21cf63a69b59609` |
| [`drivers/ata/ata_piix.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/ata_piix.c) | GPL-2.0-or-later; G2 | 1788 | `093b940bc953f0dc40b32822a2e94ac97aacd3c4` |
| [`drivers/ata/pata_ali.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_ali.c) | Legacy unversioned GPL (no SPDX); G2 | 652 | `bb790edd603611f158be7c8e59a91dadc5949f00` |
| [`drivers/ata/pata_amd.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_amd.c) | GPL-2.0-only; G2 | 642 | `5b02b89748b7fcef13c86d3a6034587099fbb29d` |
| [`drivers/ata/pata_hpt366.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_hpt366.c) | GPL-2.0-only; G2 | 461 | `5280e9960025ec02b131c67feeb724c06268b7a6` |
| [`drivers/ata/pata_hpt37x.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_hpt37x.c) | GPL-2.0-only; G2 | 1032 | `4af22b819416c70db2a601e71094474df5243af5` |
| [`drivers/ata/pata_pdc2027x.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_pdc2027x.c) | GPL-2.0-or-later; G2 | 760 | `6820c5597b14580cbec11f619c691e0da71582bb` |
| [`drivers/ata/pata_pdc202xx_old.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_pdc202xx_old.c) | GPL-2.0-only; G2 | 391 | `a32723e46357da66cc208dd65ef394d0b4e4a558` |
| [`drivers/ata/pata_sis.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_sis.c) | GPL-2.0-only; G2 | 915 | `31de06b66221056dfa9a4dda39a7bbf0d4f80d25` |
| [`drivers/ata/pata_via.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_via.c) | GPL-2.0-only; G2 | 708 | `696b99720dcbdaa94cb6542b3fc62c398f8d2330` |
| [`drivers/block/floppy.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/block/floppy.c) | GPL-2.0-only; G2 | 5037 | `3affb538b9890da73b2b99bc3bb9de0fc6e4c94e` |
| [`drivers/hid/hid-core.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-core.c) | GPL-2.0-or-later; G2 | 3101 | `582fd234eec7892c3e0f0a64884d4c9bd054cc68` |
| [`drivers/hid/hid-input.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-input.c) | GPL-2.0-or-later; G2 | 2393 | `fda9dce3da99808e04ce66dd7beec6731173e75f` |
| [`drivers/hid/usbhid/hid-core.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/usbhid/hid-core.c) | GPL-2.0-or-later; G2 | 1716 | `a9e85bdd4cc6560ec9e050e2e30ea752d93a200d` |
| [`drivers/input/gameport/gameport.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/gameport/gameport.c) | GPL-2.0-only; G2 | 885 | `10cc95867415a571a6ccdda8edf6788770791b0c` |
| [`drivers/input/gameport/ns558.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/gameport/ns558.c) | GPL-2.0-or-later; G2 | 267 | `880e714b49bc96ba3f558787a148424989f4a38b` |
| [`drivers/input/joystick/analog.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/joystick/analog.c) | GPL-2.0-or-later; G2 | 705 | `c709b58d770a2c99765179a0e862603aa9efc234` |
| [`drivers/input/mouse/alps.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/alps.c) | GPL-2.0-only; G2 | 3238 | `4e37fc3f1a9e97e0cc5cf298ca3778ca25841055` |
| [`drivers/input/mouse/alps.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/alps.h) | GPL-2.0-only; G2 | 329 | `0a1048cf23f6abeaaf554224f0c30cce7abbf2d3` |
| [`drivers/input/mouse/psmouse-base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/psmouse-base.c) | GPL-2.0-only; G2 | 2072 | `a2c9f7144864e4725e377c1ad904e7dc6f873e9b` |
| [`drivers/input/mouse/psmouse.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/psmouse.h) | GPL-2.0; G2 | 251 | `4d8acfe0d82aa3017413628d5a8ba8f917ec080f` |
| [`drivers/input/mouse/sermouse.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/sermouse.c) | GPL-2.0-or-later; G2 | 340 | `218c8432a13bc9addfe54a8818ee3701f7dd6795` |
| [`drivers/input/mouse/synaptics.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/synaptics.c) | GPL-2.0-only; G2 | 1910 | `380aa1614442f42bd1c01c7f9bdf5de256088b33` |
| [`drivers/input/mouse/synaptics.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/synaptics.h) | GPL-2.0-only; G2 | 214 | `08533d1b1b16fc2c924fca4cc2759e18b67a2fd6` |
| [`drivers/input/serio/serport.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/serio/serport.c) | GPL-2.0-only; G2 | 309 | `5a2b5404ffc21e830293895f7aaba74bea93be3b` |
| [`drivers/parport/ieee1284_ops.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/parport/ieee1284_ops.c) | GPL-2.0; G2 | 893 | `17061f1df0f441a0ac88462e5ff4691049cd4f99` |
| [`drivers/parport/parport_pc.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/parport/parport_pc.c) | GPL-2.0-only; G2 | 3408 | `f33b5d1ddfc16f5f72dfbf32d022e5f29fcebb93` |
| [`drivers/pnp/isapnp/core.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/pnp/isapnp/core.c) | GPL-2.0-or-later; G2 | 1069 | `c43d8ad0252942744381d685a9faf7cd1ad6f0ea` |
| [`drivers/tty/serial/8250/8250_pci.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/serial/8250/8250_pci.c) | GPL-2.0; G2 | 6143 | `6709b6a5f3011db38acc58dc7223158fe4fcf72e` |
| [`drivers/tty/serial/8250/8250_pnp.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/serial/8250/8250_pnp.c) | GPL-2.0; G2 | 547 | `7c06ae79d8e238aa1deaa2e383936ff246de3df4` |
| [`drivers/tty/serial/8250/8250_port.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/serial/8250/8250_port.c) | GPL-2.0+; G2 | 3477 | `3509af7dc52b8816f2f5ab58f0d999696f7483e7` |
| [`drivers/usb/host/ehci-hcd.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-hcd.c) | GPL-2.0+; G2 | 1410 | `cbc0b86fcc365e145852fd9dd9140c0fbb983e4b` |
| [`drivers/usb/host/ehci-hub.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-hub.c) | GPL-2.0+; G2 | 1222 | `1aee392e84927cba6403096ce4dff91d26470cc7` |
| [`drivers/usb/host/ehci-mem.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-mem.c) | GPL-2.0+; G2 | 224 | `4c6c08b675b5c70f53c3815a460cd1b90d929678` |
| [`drivers/usb/host/ehci-pci.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-pci.c) | GPL-2.0+; G2 | 447 | `889dc4426271173f5a5bcb7a445d7089426284b1` |
| [`drivers/usb/host/ehci-q.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-q.c) | GPL-2.0+; G2 | 1526 | `ba37a9fcab92b8a84a78a02c953cea9d3762526f` |
| [`drivers/usb/host/ehci-sched.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-sched.c) | GPL-2.0+; G2 | 2491 | `7e834587e7de1c5812e96bd3be88139d5e105eb2` |
| [`drivers/usb/host/ehci-timer.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-timer.c) | GPL-2.0+; G2 | 426 | `a79c8ac0a55fad1835f3feff687ae2a8bb0c7158` |
| [`drivers/usb/host/ehci.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci.h) | GPL-2.0+; G2 | 913 | `d7a3c8d13f6b8a873c8997af85f562db7fc712e3` |
| [`drivers/usb/host/ohci-pci.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ohci-pci.c) | GPL-1.0+; G2 | 335 | `900ea0d368e0343e8da6f5e418d2f43a206aef65` |
| [`drivers/usb/host/pci-quirks.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/pci-quirks.c) | GPL-2.0; G2 | 1297 | `1f9c1b1435d862895b3c3bee40760d4ceccf0a3e` |
| [`drivers/usb/host/pci-quirks.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/pci-quirks.h) | GPL-2.0; G2 | 50 | `a5230b0b9e913a920e2b42b9f5b43ece0866ce67` |
| [`drivers/usb/host/uhci-pci.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/uhci-pci.c) | GPL-2.0; G2 | 318 | `5edf6a08cf82c670c031cc3daf8b2ff39f7db1b3` |
| [`include/linux/pci_ids.h`](https://github.com/torvalds/linux/blob/v6.12/include/linux/pci_ids.h) | GPL-2.0; G2 | 3236 | `4cf6aaed5f35db0bbafb8ec94a729a21fe2d29e5` |
| [`include/linux/serio.h`](https://github.com/torvalds/linux/blob/v6.12/include/linux/serio.h) | GPL-2.0-only; G2 | 164 | `bf2191f2535093289fdddab50f2b2029b2340056` |
| [`include/sound/ac97_codec.h`](https://github.com/torvalds/linux/blob/v6.12/include/sound/ac97_codec.h) | GPL-2.0+; G2 | 421 | `4bd3be3a3192af27afcab6d1885e981507eee039` |
| [`include/sound/emu10k1.h`](https://github.com/torvalds/linux/blob/v6.12/include/sound/emu10k1.h) | GPL-2.0-or-later; G2 | 1922 | `38db50b280eba5ad72100a0a44798d6ca2d5db75` |
| [`include/uapi/linux/serial_reg.h`](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/serial_reg.h) | GPL-1.0+ WITH Linux-syscall-note; G2 | 387 | `9c987b04e2d04b4a933f9b09e2b31af5b6fde95d` |
| [`include/uapi/linux/serio.h`](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/serio.h) | GPL-2.0 WITH Linux-syscall-note; G2 | 88 | `5a2af0942c9f24393ca4e38002855bd8ea8c4cd9` |
| [`sound/pci/ac97/ac97_codec.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_codec.c) | GPL-2.0-or-later; G2 | 3006 | `25f93e56cfc7a5c93e1ff51c32781f36b2dd12e5` |
| [`sound/pci/ac97/ac97_local.h`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_local.h) | GPL-2.0-or-later; G2 | 26 | `8eeae2dec552a778fedb2f374b0e20ab0f0bee4f` |
| [`sound/pci/ac97/ac97_patch.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_patch.c) | GPL-2.0-or-later; G2 | 3918 | `cd83aa864ea3e6e966140803a19818d663c49d72` |
| [`sound/pci/ac97/ac97_patch.h`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_patch.h) | GPL-2.0-or-later; G2 | 80 | `a8cd89cfd6ff66d34cbdce2507680892a4b29c18` |
| [`sound/pci/ali5451/ali5451.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ali5451/ali5451.c) | GPL-2.0-or-later; G2 | 2166 | `793d2f13267ea1212c30b0ff3e4e941f53ad08fb` |
| [`sound/pci/cmipci.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/cmipci.c) | GPL-2.0-or-later; G2 | 3321 | `e3cac73517d68eea50d9b7f7d2457ab759919ffe` |
| [`sound/pci/emu10k1/emu10k1_main.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emu10k1_main.c) | GPL-2.0-or-later; G2 | 1797 | `5b8a5ba825bdb2555cec7a13def744b062b50fa5` |
| [`sound/pci/emu10k1/emufx.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emufx.c) | GPL-2.0-or-later; G2 | 2744 | `03efc317e05f78d67d2ec9ce12c9e087a0bd05e2` |
| [`sound/pci/emu10k1/emumixer.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emumixer.c) | GPL-2.0-or-later; G2 | 2390 | `05b98d9b547b2a9ffd063649311887693b1b20de` |
| [`sound/pci/emu10k1/emupcm.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emupcm.c) | GPL-2.0-or-later; G2 | 1854 | `1bf6e3d652f81448a563edc9c930b84eadcef5f0` |
| [`sound/pci/emu10k1/io.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/io.c) | GPL-2.0-or-later; G2 | 771 | `b60ab5671e00dccbf77397fce3cae8fe37ca78e3` |
| [`sound/pci/emu10k1/irq.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/irq.c) | GPL-2.0-or-later; G2 | 162 | `71aa90b9cc88875dffcbeea75ccc4e4e453ff6d0` |
| [`sound/pci/emu10k1/memory.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/memory.c) | GPL-2.0-or-later; G2 | 663 | `d29711777161c3559bd95d72599c74c3d157b84e` |
| [`sound/pci/emu10k1/voice.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/voice.c) | GPL-2.0-or-later; G2 | 140 | `77fb5427aaed587092f0aa1ca26a1f0939b20836` |
| [`sound/pci/es1968.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/es1968.c) | GPL-2.0-or-later; G2 | 2846 | `c6c018b40c69f9bd1af9c4d361cb6258ae01b077` |
| [`sound/pci/intel8x0.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/intel8x0.c) | GPL-2.0-or-later; G2 | 3199 | `e4bb99f71c2c9e98e31cc240ad31e54519186d70` |
| [`sound/pci/trident/trident.h`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/trident/trident.h) | GPL-2.0-or-later; G2 | 422 | `ed2d4eecc704affad9dca73e906d81dbb4305c44` |
| [`sound/pci/trident/trident_main.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/trident/trident_main.c) | GPL-2.0-or-later; G2 | 3885 | `e98eea1e6d81e0d5fe123792c6b6f659b77102f5` |
| [`sound/pci/ymfpci/ymfpci.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ymfpci/ymfpci.c) | GPL-2.0-or-later; G2 | 366 | `48444dda44defdad3ee3b30bd3ba40d3555111c1` |
| [`sound/pci/ymfpci/ymfpci.h`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ymfpci/ymfpci.h) | GPL-2.0-or-later; G2 | 410 | `a408785cfa1ba451df5d806fb3dcd478bf5f5fe4` |
| [`sound/pci/ymfpci/ymfpci_main.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ymfpci/ymfpci_main.c) | GPL-2.0-or-later; G2 | 2357 | `6b8d8690b6b204eee0cad44e74c7c5afd380d230` |

The EMU10K1 files include professional E-MU FPGA loaders; those external
firmware branches are excluded from the Live!/Audigy PCM selection. The Yamaha
Linux driver requests separate microcode. Neither driver license grants rights
to unrelated firmware payloads. No Microsoft/Apple driver artwork or binary is
part of this manifest.

## ACPICA 20260930 runtime

Upstream: [release 20260930](https://github.com/open-acpica/acpica/releases/tag/20260930),
source tree identity `e85aa3bebaae81ed1e6d163511ac7e1eafe732cf`.
The nine runtime component directories below contain **164 C units**; the
selected root include headers plus `platform/acenv.h`, `acenvex.h` and `acgcc.h`
contain **43 headers**. Their upstream tree reports **3,499,399 bytes** in total.
Platform selection will use new Ciuki headers, not accidentally select Linux
OS glue during a host build. Build-time feature exclusions and final link map
must be checked in the implementation; this census claims no compiled size.

The [BSD-3-Clause text](https://github.com/open-acpica/acpica/blob/20260930/LICENSE.BSD-3-Clause)
and [GPL-2.0-only text](https://github.com/open-acpica/acpica/blob/20260930/LICENSE.GPL-2.0-only)
were read. The release's simplified dual-license description has **12 file-level
exceptions** in this set. Their complete retained header offers the older Intel
license, a custom BSD-like alternative with downstream binary-disclaimer terms,
and explicitly GPL version 2. Select the GPL v2 alternative. No need to rely on
or reclassify the custom permissive option.

Verdicts in the following complete manifest:

- **A2**: header `BSD-3-Clause OR GPL-2.0-only`; choose GPL-2.0-only, compatible.
- **A2L**: older multi-alternative header, no SPDX; choose its explicit GPL v2
  alternative, compatible. These are all twelve legacy exceptions.

| Exact candidate path | Verdict | Text lines | Git blob SHA-1 |
| --- | --- | ---: | --- |
| [`source/components/dispatcher/dsargs.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsargs.c) | A2 | 410 | `c50e089a788b724d84b961f717fc7409b43ff493` |
| [`source/components/dispatcher/dscontrol.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dscontrol.c) | A2 | 388 | `76757801e6dd2f74e444b476d3032be0cee8a8b5` |
| [`source/components/dispatcher/dsdebug.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsdebug.c) | A2 | 227 | `2932e6e226579da131c0a1c8ea14473bdf5ef999` |
| [`source/components/dispatcher/dsfield.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsfield.c) | A2 | 878 | `dae5977cf3281fc1e7c9441195d8106156156d88` |
| [`source/components/dispatcher/dsinit.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsinit.c) | A2 | 244 | `707d1f5cd8f2c0571100f0522ab15d070fec170d` |
| [`source/components/dispatcher/dsmethod.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsmethod.c) | A2 | 958 | `caad1154375f8fb7f31fc61854b09e7dbba42ef6` |
| [`source/components/dispatcher/dsmthdat.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsmthdat.c) | A2 | 749 | `26afeab9c6ce9dcf4dc6d1ae54f3131acee35811` |
| [`source/components/dispatcher/dsobject.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsobject.c) | A2 | 560 | `a4a2bfefbcb9b0ddac62802305850f7f818cb27c` |
| [`source/components/dispatcher/dsopcode.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsopcode.c) | A2 | 812 | `e5cf93853962816a98827b9711799b921c67deaf` |
| [`source/components/dispatcher/dspkginit.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dspkginit.c) | A2 | 564 | `a5052f1189539731694de207b399c138b42187ea` |
| [`source/components/dispatcher/dsutils.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dsutils.c) | A2 | 917 | `93b84d54faf4377090c9960703510ea68fb62aab` |
| [`source/components/dispatcher/dswexec.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dswexec.c) | A2 | 790 | `8098d35a3e753401847f71be66bcc43ee99ba161` |
| [`source/components/dispatcher/dswload.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dswload.c) | A2 | 615 | `6b88587d0e63d377a160cbc0c2470cb7e37ace8a` |
| [`source/components/dispatcher/dswload2.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dswload2.c) | A2 | 770 | `e1738c7b593418b8c77ceda7a435e7db370a6218` |
| [`source/components/dispatcher/dswscope.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dswscope.c) | A2 | 203 | `9dd6d7b70f2ed2e4926562f884da5597f7514a1a` |
| [`source/components/dispatcher/dswstate.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/dispatcher/dswstate.c) | A2 | 817 | `9cfa43455acd6faef02e01d0fbee4aa21e27c7c3` |
| [`source/components/events/evevent.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evevent.c) | A2 | 311 | `b42bc78bcf51562f692c91e43d07d955c84314f6` |
| [`source/components/events/evglock.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evglock.c) | A2 | 354 | `8db3d3bdba8cbbdfdb0bc7131b85775b49eb6819` |
| [`source/components/events/evgpe.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evgpe.c) | A2 | 1034 | `f64bc021c9b7ce92547f498c02df25873adb8765` |
| [`source/components/events/evgpeblk.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evgpeblk.c) | A2 | 539 | `f78ffc8ad6cc503166186c84c67b8f18081d3c8e` |
| [`source/components/events/evgpeinit.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evgpeinit.c) | A2 | 437 | `e722501f34b8b4ed6901fe1ab19ed2e0c5cd4515` |
| [`source/components/events/evgpeutil.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evgpeutil.c) | A2 | 368 | `fc28a911af2ac0cb1f0e7bcf92a2635d91d7e3fc` |
| [`source/components/events/evhandler.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evhandler.c) | A2 | 595 | `37629f17dbe1c0e10034f9a6feac6c9df1ac4431` |
| [`source/components/events/evmisc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evmisc.c) | A2 | 346 | `d96558a43c1e21bb6244c6d0f6aa1a6b2797e011` |
| [`source/components/events/evregion.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evregion.c) | A2 | 946 | `a08ff6d624e26d8c50b0395b11d9ebf27db8311d` |
| [`source/components/events/evrgnini.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evrgnini.c) | A2 | 693 | `9ebdad10558dfb433646ca34b02b68ea03314d9d` |
| [`source/components/events/evsci.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evsci.c) | A2L | 391 | `709c81776062a3cc9f2339f1cea8cbdf3b27403d` |
| [`source/components/events/evxface.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evxface.c) | A2 | 1238 | `b8e7fcc84036400f973e198a0bce9937dad78cab` |
| [`source/components/events/evxfevnt.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evxfevnt.c) | A2 | 421 | `f0ecc5e7e699c7bec761d7f223b7c1d47d7d4100` |
| [`source/components/events/evxfgpe.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evxfgpe.c) | A2 | 1217 | `0c7e1814e09c1a238595a9d64a11ff7b458ec114` |
| [`source/components/events/evxfregn.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/events/evxfregn.c) | A2 | 339 | `f064c2f7bebbdc622ed77af5c86e10882f26e74e` |
| [`source/components/executer/exconcat.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exconcat.c) | A2L | 570 | `93f114b8cd03aedcb8d4675bf4f3362bd475069a` |
| [`source/components/executer/exconfig.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exconfig.c) | A2L | 769 | `278b5622e6aa137bca48c2ee7230c3750cad7e2b` |
| [`source/components/executer/exconvrt.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exconvrt.c) | A2 | 786 | `76eccce2eb6339763eb5b043b6f5fa5c904a7259` |
| [`source/components/executer/excreate.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/excreate.c) | A2L | 660 | `f78c7e53f69b0ee101423562190afa05b4497e5d` |
| [`source/components/executer/exdebug.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exdebug.c) | A2 | 323 | `80350f1843b06035f19a2d68e0550b9040aced37` |
| [`source/components/executer/exdump.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exdump.c) | A2 | 1282 | `bfc5069501110aaea2e45f3c6381175d97dff990` |
| [`source/components/executer/exfield.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exfield.c) | A2 | 465 | `25a5904c38fedeb473c3d910beebfe999073ea31` |
| [`source/components/executer/exfldio.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exfldio.c) | A2 | 1018 | `091e1be4e3ab92ba464629817dbc67c1520ee1de` |
| [`source/components/executer/exmisc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exmisc.c) | A2L | 643 | `befd8a998e802004e05757fa32c7ae4ccd530c2d` |
| [`source/components/executer/exmutex.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exmutex.c) | A2 | 555 | `4c06df6fdad55a9ce25d7c2008fed899d56f935c` |
| [`source/components/executer/exnames.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exnames.c) | A2 | 447 | `8ce650eddb214f2532efa19ae1d3e7e20896d452` |
| [`source/components/executer/exoparg1.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exoparg1.c) | A2 | 1131 | `0de6b496e8a32566e6ad01a1655b1cb0ff58440e` |
| [`source/components/executer/exoparg2.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exoparg2.c) | A2 | 612 | `9f36ed0ab579f82c38860517e152219a02352fff` |
| [`source/components/executer/exoparg3.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exoparg3.c) | A2L | 417 | `e42317ab303922ff1c994e4f1fa1dbd3aaa77ef0` |
| [`source/components/executer/exoparg6.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exoparg6.c) | A2L | 465 | `85c1e2db839ef6f2843c608b4e5f60ddde60eda0` |
| [`source/components/executer/exprep.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exprep.c) | A2 | 637 | `e3d77140a0d5cf47c1f21ebc8aa6f25c1503d534` |
| [`source/components/executer/exregion.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exregion.c) | A2 | 571 | `8a22f8ebf3a1d9c4f4f70ce5969bd2a32ab32d89` |
| [`source/components/executer/exresnte.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exresnte.c) | A2 | 262 | `0bb8b92870fcec61f38e34d79aa8f229c39a1c7d` |
| [`source/components/executer/exresolv.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exresolv.c) | A2 | 563 | `d16cc61f814592e7a13570137547662130ffea1c` |
| [`source/components/executer/exresop.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exresop.c) | A2 | 702 | `5780bb4a93061372a09f6aaaf55b5305bdecbfa9` |
| [`source/components/executer/exserial.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exserial.c) | A2 | 393 | `1b12b3bdef926e412d8e2ffbe3b70f5f2e0235a8` |
| [`source/components/executer/exstore.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exstore.c) | A2 | 630 | `a00516329d06bbf46c5f6b01b402f166f5d0a6e5` |
| [`source/components/executer/exstoren.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exstoren.c) | A2 | 274 | `89dc04fc5fbe8f279c85c2f39ada864c806f7693` |
| [`source/components/executer/exstorob.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exstorob.c) | A2 | 210 | `746302f29f21dedbe7667003841a27d2d7cc188f` |
| [`source/components/executer/exsystem.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exsystem.c) | A2 | 324 | `2369b1be7672055cba40c323d6c6048aa939436c` |
| [`source/components/executer/extrace.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/extrace.c) | A2 | 431 | `7952ed8d066470bef718d21b0e6e16b7b05eb907` |
| [`source/components/executer/exutils.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/executer/exutils.c) | A2 | 477 | `bdb1a1a493a3cfd930451712294e76e9e1a07a7d` |
| [`source/components/hardware/hwacpi.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwacpi.c) | A2 | 191 | `1b599e82e2278b1faa151409c2b680e3ed0d6514` |
| [`source/components/hardware/hwesleep.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwesleep.c) | A2 | 238 | `4d3df12b31e01b9b746e631e401bb7dd018d008b` |
| [`source/components/hardware/hwgpe.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwgpe.c) | A2 | 659 | `b02139f1998c14a64e65aae71ee5cf4e005fcfcd` |
| [`source/components/hardware/hwpci.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwpci.c) | A2 | 433 | `00c3c809a2ee11da97e135696564912e7369e6da` |
| [`source/components/hardware/hwregs.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwregs.c) | A2 | 874 | `8e6fa866c225edeeaf47f619a47ea77129bcce54` |
| [`source/components/hardware/hwsleep.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwsleep.c) | A2 | 344 | `4dd6697cba159386911b6d01ae1bb3c3fc0d5f5f` |
| [`source/components/hardware/hwtimer.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwtimer.c) | A2 | 210 | `29bec9e221c774be4122479e0c955fad1fa1c86d` |
| [`source/components/hardware/hwvalid.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwvalid.c) | A2 | 334 | `70b800ab6e9b0a527dc73f06da7fd2c6d22c6680` |
| [`source/components/hardware/hwxface.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwxface.c) | A2 | 508 | `4797d6214a68d06ef123cf1e5a4de2523d0f0f16` |
| [`source/components/hardware/hwxfsleep.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/hardware/hwxfsleep.c) | A2 | 485 | `16202d6e686e4a422f3067fa29027039a1a9a930` |
| [`source/components/namespace/nsaccess.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsaccess.c) | A2 | 781 | `f0f881f6cf4e0065f8b99847cd097f8ad95fa126` |
| [`source/components/namespace/nsalloc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsalloc.c) | A2 | 570 | `61a6c14773514830bc04126e4332f1bc63effb16` |
| [`source/components/namespace/nsarguments.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsarguments.c) | A2 | 291 | `6db9aa2b6cf1b561f28b9e89f368580119747215` |
| [`source/components/namespace/nsconvert.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsconvert.c) | A2 | 536 | `ce8a6a91b3a8c3ea69790eb8db980ca63b5d2243` |
| [`source/components/namespace/nsdump.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsdump.c) | A2L | 1058 | `c44c9ccfde7dd96952c27f6215ef45598b478d8f` |
| [`source/components/namespace/nsdumpdv.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsdumpdv.c) | A2L | 265 | `a3d75f44a3eebdd33127931b4b1714ad319c5aab` |
| [`source/components/namespace/nseval.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nseval.c) | A2L | 460 | `a46c6b92f556309acdab9605fa9e6b5691241d38` |
| [`source/components/namespace/nsinit.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsinit.c) | A2 | 739 | `345a60245fac014afc191a0dc062ccb551176e35` |
| [`source/components/namespace/nsload.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsload.c) | A2 | 328 | `3db532c8596081f7fb70af4e75b49b7aca0e2b66` |
| [`source/components/namespace/nsnames.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsnames.c) | A2 | 547 | `0f4915154e1694b5acbbd23df68e2b9b0b5b85f2` |
| [`source/components/namespace/nsobject.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsobject.c) | A2 | 499 | `b4dec85aacce8e8c84de5d593fbe1c5cdcd0996d` |
| [`source/components/namespace/nsparse.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsparse.c) | A2 | 301 | `ac6853050892b38311fdf24f88d7d146cc382ced` |
| [`source/components/namespace/nspredef.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nspredef.c) | A2 | 406 | `644d5d91461aef1425dc73959949deed5c0b6137` |
| [`source/components/namespace/nsprepkg.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsprepkg.c) | A2 | 769 | `15b7a961e9208d1c55cddaf5ad80a07034b319d5` |
| [`source/components/namespace/nsrepair.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsrepair.c) | A2 | 601 | `6c22f3851c863a60ad4e08e0a0939ba6eb057b95` |
| [`source/components/namespace/nsrepair2.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsrepair2.c) | A2 | 1052 | `526b8d175fbed7b30d4e1802df305552bd49eb20` |
| [`source/components/namespace/nssearch.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nssearch.c) | A2 | 423 | `c2395860d2c01b26f342e954e2a7491ea1cee9ed` |
| [`source/components/namespace/nsutils.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsutils.c) | A2 | 856 | `045b4ad67fb8aaae22b7d6150b8ae7c441684283` |
| [`source/components/namespace/nswalk.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nswalk.c) | A2 | 358 | `ddd0d15112fdef89272205d4c4ec4055a4889d1b` |
| [`source/components/namespace/nsxfeval.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsxfeval.c) | A2 | 1075 | `e39b360bd126520023485190eaa39c973670f9af` |
| [`source/components/namespace/nsxfname.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsxfname.c) | A2 | 689 | `4c7c1a4612600063fff1ccb7c5c96c5cee435be3` |
| [`source/components/namespace/nsxfobj.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/namespace/nsxfobj.c) | A2 | 251 | `4b0f5441b2dbd4586d24eab3048cf093da3c5120` |
| [`source/components/parser/psargs.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psargs.c) | A2 | 1161 | `8ef957d88f0a5f60cb24bef512d42230b6e10999` |
| [`source/components/parser/psloop.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psloop.c) | A2 | 574 | `e27beb6dd1ed9127cfe538301ad0ba2e4b5914d2` |
| [`source/components/parser/psobject.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psobject.c) | A2 | 748 | `d1c7e4a161edfffb2c560f2cbc20e9c9822813b4` |
| [`source/components/parser/psopcode.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psopcode.c) | A2 | 314 | `997f85741a865b00743eff82ecf1df0a67805144` |
| [`source/components/parser/psopinfo.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psopinfo.c) | A2 | 254 | `511d7d467b3623ddc11ec1d0badde1a2c45c01b2` |
| [`source/components/parser/psparse.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psparse.c) | A2 | 722 | `1aacc0c0ae9f516d6ff388e86fca5f3ea18e4ebb` |
| [`source/components/parser/psscope.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psscope.c) | A2 | 270 | `bcab07f77148de139199ef5cc2e3253584ef7c9b` |
| [`source/components/parser/pstree.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/pstree.c) | A2 | 333 | `ca4a2e68eea5f6978d324d2865bef7e6249f6ceb` |
| [`source/components/parser/psutils.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psutils.c) | A2 | 264 | `0ec46c442389bb8a7d46a725d3c9b85053af6279` |
| [`source/components/parser/pswalk.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/pswalk.c) | A2 | 116 | `2ebcefa918c0b86d22f7d5a1e60f1718c6261518` |
| [`source/components/parser/psxface.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/parser/psxface.c) | A2 | 380 | `0fe6ddd5a7ea18e5d4c04d701665fcbcc4a26dbc` |
| [`source/components/resources/rsaddr.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsaddr.c) | A2 | 381 | `06f8d2060353aff25e4462f656f8503706df1881` |
| [`source/components/resources/rscalc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rscalc.c) | A2 | 855 | `caf43d4281cda06d49e518763c527206d1b8cb05` |
| [`source/components/resources/rscreate.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rscreate.c) | A2 | 480 | `2b59402012c7e5f94da89f06c547ef93768876af` |
| [`source/components/resources/rsdump.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsdump.c) | A2 | 695 | `bcf1a6bc9892ef0a9933757e8a78d3a1e5701ce4` |
| [`source/components/resources/rsdumpinfo.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsdumpinfo.c) | A2 | 424 | `4184c7ccfc962237670114a5c15df95733652c95` |
| [`source/components/resources/rsinfo.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsinfo.c) | A2 | 266 | `592f126ccff2d78b331ccddef72da961528fe74d` |
| [`source/components/resources/rsio.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsio.c) | A2 | 270 | `1a20ce0e66223b8a0e7adc310b5756df7a6d736e` |
| [`source/components/resources/rsirq.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsirq.c) | A2 | 288 | `3508c7c5ba3798157b5ee464c7f62908db7d0cfd` |
| [`source/components/resources/rslist.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rslist.c) | A2 | 258 | `9f2456ca99646170f53c0b188c6e39b7cdd12722` |
| [`source/components/resources/rsmemory.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsmemory.c) | A2 | 217 | `d81e3ebaac46dc45b3acd370a99b02fa33ee9529` |
| [`source/components/resources/rsmisc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsmisc.c) | A2 | 805 | `6776f55a4754371797dc92464f03981cbbfd23a6` |
| [`source/components/resources/rsserial.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsserial.c) | A2 | 875 | `ea68372b13b2b5114f19aa4781b96ee2278068cb` |
| [`source/components/resources/rsutils.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsutils.c) | A2 | 840 | `591e88622e4b4f302dadac7c56833e626fe2458c` |
| [`source/components/resources/rsxface.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/resources/rsxface.c) | A2 | 714 | `9ede619494bd9b1b48f6f43d1f41dc9e50faed1d` |
| [`source/components/tables/tbdata.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbdata.c) | A2 | 1299 | `0d57f132cca252d9fa143d4af406818aeb4734bd` |
| [`source/components/tables/tbfadt.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbfadt.c) | A2 | 758 | `9f83077a5b2b16556f027c65d9f5898d14ed51b0` |
| [`source/components/tables/tbfind.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbfind.c) | A2 | 147 | `a8d644c38deafccaefd881e2b2a7923b68eb6db5` |
| [`source/components/tables/tbinstal.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbinstal.c) | A2 | 331 | `90c28fcba7fc1867faa0a3131348de153da0ab69` |
| [`source/components/tables/tbprint.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbprint.c) | A2 | 171 | `1ad28045b9ad57ba7174cc9edf26c2064cd749ab` |
| [`source/components/tables/tbutils.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbutils.c) | A2 | 483 | `f4a81f962c871d10c9618c42b293c932e099c8d6` |
| [`source/components/tables/tbxface.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbxface.c) | A2 | 609 | `e407d700eedca6ff033ac7c5f526fdab460a89c4` |
| [`source/components/tables/tbxfload.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbxfload.c) | A2 | 514 | `b88643cb6c84c2a15a7a18f39ebb54b6172c07d5` |
| [`source/components/tables/tbxfroot.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbxfroot.c) | A2 | 307 | `89f9b7750ec02752e0aaec8a07da4075374fca4c` |
| [`source/components/utilities/utaddress.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utaddress.c) | A2 | 285 | `5f8cd2fd5aede46393e8c0f1d3b55cccfe31729f` |
| [`source/components/utilities/utalloc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utalloc.c) | A2 | 379 | `5ab3c3e161d25bc55c8a5751213044cfda957697` |
| [`source/components/utilities/utascii.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utascii.c) | A2 | 131 | `8e60ba0034e461a79db5c38fef1b90082e3f78ca` |
| [`source/components/utilities/utbuffer.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utbuffer.c) | A2 | 340 | `03bb31d86bc5dadcd11e31258bdfd0f39f35453b` |
| [`source/components/utilities/utcache.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utcache.c) | A2 | 324 | `a10f705bf1406afab818ecea550d65222735993a` |
| [`source/components/utilities/utcksum.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utcksum.c) | A2 | 197 | `ef8893f52ca74f49604c66840702b76335a23a15` |
| [`source/components/utilities/utclib.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utclib.c) | A2 | 1030 | `3566a72fcefdcc55c36c3896614580df0873a7a1` |
| [`source/components/utilities/utcopy.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utcopy.c) | A2 | 1055 | `49ca1267b3f5f6400e25c7755ec5ec8713f43f4d` |
| [`source/components/utilities/utdebug.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utdebug.c) | A2 | 705 | `fc3279242fbc38f9c7a44a1c6f81d4fe6b9fe71d` |
| [`source/components/utilities/utdecode.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utdecode.c) | A2 | 663 | `4767c24fa4df839ef7eb4cb5b9a04280779d7040` |
| [`source/components/utilities/utdelete.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utdelete.c) | A2 | 800 | `243ee65dfda197539fb5a388cb4bb033e9ee6569` |
| [`source/components/utilities/uterror.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/uterror.c) | A2 | 372 | `bd12ef740920994a11d644ce6876c38d95ac9427` |
| [`source/components/utilities/uteval.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/uteval.c) | A2 | 349 | `8f5ad824215cd66e99bf576706f8141e88cb32be` |
| [`source/components/utilities/utexcep.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utexcep.c) | A2 | 149 | `092b76d1df19c4cde42e04e2df78710f0c8eb464` |
| [`source/components/utilities/utglobal.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utglobal.c) | A2 | 210 | `bab3bff4b681bea272c987ca0f36a2c5edee2361` |
| [`source/components/utilities/uthex.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/uthex.c) | A2 | 126 | `d969043e49475c01244df611f487875d8629ccb4` |
| [`source/components/utilities/utids.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utids.c) | A2 | 448 | `3ad3624e565949e227088de5599445f8ea9b3dc1` |
| [`source/components/utilities/utinit.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utinit.c) | A2 | 321 | `2ce64c0cbd4d99ba99d6305a597dd7a7cb6fdb5b` |
| [`source/components/utilities/utlock.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utlock.c) | A2 | 172 | `a43876914a8653798186e82f6614d35c402912a5` |
| [`source/components/utilities/utmath.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utmath.c) | A2 | 586 | `ecd46df3f07ba409595259be3722d82076219678` |
| [`source/components/utilities/utmisc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utmisc.c) | A2 | 435 | `927b802f14b6474f9be24176a26925a8073085a2` |
| [`source/components/utilities/utmutex.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utmutex.c) | A2 | 387 | `b12a702b41fcb6ba9072a0e603f18225de548f2b` |
| [`source/components/utilities/utnonansi.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utnonansi.c) | A2 | 222 | `3fc6b5bbf2f94a9d0ed2fb0efe1e8c35c948e1c5` |
| [`source/components/utilities/utobject.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utobject.c) | A2 | 754 | `d851a4d415a3839726395702d9d003dc541013e7` |
| [`source/components/utilities/utosi.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utosi.c) | A2 | 533 | `e0975869899d3ce12962f4ca23a5e435f73bb67b` |
| [`source/components/utilities/utownerid.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utownerid.c) | A2 | 215 | `4d896684f62a2085a94270cdd607d067718de923` |
| [`source/components/utilities/utpredef.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utpredef.c) | A2 | 420 | `c820305e87039d80eace72489c2ea0945c428a94` |
| [`source/components/utilities/utprint.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utprint.c) | A2L | 1016 | `3d26ab54a1411d24e0dfcfc87669755373a17bf3` |
| [`source/components/utilities/utresdecode.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utresdecode.c) | A2 | 345 | `d4f9130480621c0d6a833dfe15e7ccb2b7e3c097` |
| [`source/components/utilities/utresrc.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utresrc.c) | A2 | 665 | `46341cef63877e580c7504bda0ff431307d3564d` |
| [`source/components/utilities/utstate.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utstate.c) | A2 | 324 | `ebc05a43e5faa4b4487036a51970398e06ca2f47` |
| [`source/components/utilities/utstring.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utstring.c) | A2 | 247 | `9831b3ec26de617b09aeeee29ceb91f838683cb9` |
| [`source/components/utilities/utstrsuppt.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utstrsuppt.c) | A2 | 525 | `79ce2757711f24c3bebf605b40d4d0e4e654187d` |
| [`source/components/utilities/utstrtoul64.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utstrtoul64.c) | A2 | 364 | `c9fb250647d17f87bad3d113d7e941ea1a1bb555` |
| [`source/components/utilities/uttrack.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/uttrack.c) | A2 | 771 | `d5de82770650c3261eb866a1b244d42010844665` |
| [`source/components/utilities/utuuid.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utuuid.c) | A2 | 121 | `44d9e02c8aedb3fee1f08cce10b5b5158da80ca9` |
| [`source/components/utilities/utxface.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utxface.c) | A2 | 613 | `144c02bb13dbb716de98253d4543546f78c3a593` |
| [`source/components/utilities/utxferror.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utxferror.c) | A2 | 324 | `40336dbc20f4fa5f9adc5d5510083fe58be27318` |
| [`source/components/utilities/utxfinit.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utxfinit.c) | A2 | 292 | `20ee3d95560408d5c8cc9721506a74f83621ea20` |
| [`source/components/utilities/utxfmutex.c`](https://github.com/open-acpica/acpica/blob/20260930/source/components/utilities/utxfmutex.c) | A2 | 185 | `170a10e502236bdb3051d675ebc99ee8443e444b` |
| [`source/include/acapps.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acapps.h) | A2 | 205 | `394aa3f61bc928d93dcbfa66fb72a1a3372555e3` |
| [`source/include/acbuffer.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acbuffer.h) | A2 | 234 | `a4df25462c9ee2bceced119462eba34506dc0398` |
| [`source/include/acclib.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acclib.h) | A2 | 293 | `e14d746383d6aed9a2049c066b62960eb26b2dd3` |
| [`source/include/accommon.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/accommon.h) | A2 | 37 | `295ad8422c5f22123064a12792e71b53d3192a75` |
| [`source/include/acconfig.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acconfig.h) | A2 | 239 | `6d94a2646ffffb19172ac7f512f14d01d708a6c2` |
| [`source/include/acconvert.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acconvert.h) | A2 | 173 | `4e5bc3214df508920d637a9d6b55615aa4848175` |
| [`source/include/acdebug.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acdebug.h) | A2 | 488 | `eda60b600f534cf7536d0b671ee47afcdac0d3d2` |
| [`source/include/acdisasm.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acdisasm.h) | A2 | 1507 | `333d48c3480f50514d1b959856d6ebce4fc76f75` |
| [`source/include/acdispat.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acdispat.h) | A2 | 461 | `c50874b35675b40b4f11078ca21bf7e744ce10fb` |
| [`source/include/acevents.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acevents.h) | A2 | 374 | `ace3ca92bef81210125e7cd6e3390640f5b325dd` |
| [`source/include/acexcep.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acexcep.h) | A2 | 348 | `2d20618b8748bc40a7c21c782e5968de96e197fd` |
| [`source/include/acglobal.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acglobal.h) | A2 | 399 | `333a33893e1d471854a31af760d574ce465b5408` |
| [`source/include/achware.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/achware.h) | A2 | 201 | `561466dffb70aa9cc7c6d5d7dddcd713daf2a13b` |
| [`source/include/acinterp.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acinterp.h) | A2 | 758 | `bd80c471a0fdba7d6d98c07616518d710fa95933` |
| [`source/include/aclocal.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/aclocal.h) | A2 | 1527 | `d0e4b067c0539cf34a5c1d2a176a0a0570255089` |
| [`source/include/acmacros.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acmacros.h) | A2 | 509 | `6b98bafc79b2e2800d4f0c9133c8d3b547ffc1fa` |
| [`source/include/acnames.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acnames.h) | A2 | 68 | `51f486242e96bd8ae536f48f84987ddc1e1fe860` |
| [`source/include/acnamesp.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acnamesp.h) | A2 | 555 | `97c57ea851e11ecf58a4c0503ab1269a416aa204` |
| [`source/include/acobject.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acobject.h) | A2 | 569 | `7cba4041516e8c394f5a5b5992c3ad17e10f2daa` |
| [`source/include/acopcode.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acopcode.h) | A2 | 303 | `6633c9ada09202a091f0e98abf514ba9f021759f` |
| [`source/include/acoutput.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acoutput.h) | A2 | 481 | `ae58fd5bc0a4fbf6d36f836a944e172722cdc3ec` |
| [`source/include/acparser.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acparser.h) | A2 | 338 | `2d85b6f0ebe2a851ebb4be15143ce7661a2d2bc0` |
| [`source/include/acpi.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acpi.h) | A2 | 37 | `3e72807054de9bddf76c57feee60082ed6a56c04` |
| [`source/include/acpiosxf.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acpiosxf.h) | A2 | 567 | `8bb63438f329c8a08434b456bbd6d05a45a318b8` |
| [`source/include/acpixf.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acpixf.h) | A2 | 1312 | `028f94ad39f1421dae11c380daee15d4e297be7c` |
| [`source/include/acpredef.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acpredef.h) | A2 | 1171 | `26266e302847e769b7be8cbc9bb0e7e464af6270` |
| [`source/include/acresrc.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acresrc.h) | A2L | 569 | `d648194883daed75d1c50dc3bd64be38455a87b9` |
| [`source/include/acrestyp.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acrestyp.h) | A2 | 820 | `a6caba710f9fc605ae5103f96b6f6a6bbfc967f3` |
| [`source/include/acstruct.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acstruct.h) | A2 | 255 | `3dc3f505e0060177e4a51ef07e0b826cad139c3c` |
| [`source/include/actables.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actables.h) | A2 | 245 | `98c8468422dba4a6962ff59f42e1fae7a5a1b516` |
| [`source/include/actbinfo.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actbinfo.h) | A2 | 449 | `6e2db405cf594e3d75dc57895a2a5bfed405dbde` |
| [`source/include/actbl.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actbl.h) | A2 | 441 | `8eee17869d10b6a4f258941a7a2443e201ebd902` |
| [`source/include/actbl1.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actbl1.h) | A2 | 2637 | `c49bbcc7c3eedb3fef30c904fde67beb398c3a50` |
| [`source/include/actbl2.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actbl2.h) | A2 | 4429 | `c58548983459eb472bc2b936d63e4b319b5e8387` |
| [`source/include/actbl3.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actbl3.h) | A2 | 906 | `18209c15abc38c5f1c59163bdb98d46368be4532` |
| [`source/include/actypes.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/actypes.h) | A2 | 1449 | `67c97b00734fccbe01279384186497ba519c38de` |
| [`source/include/acutils.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acutils.h) | A2 | 1170 | `74ca5223b79fbce2baf9157829e4e9fa8e8ae250` |
| [`source/include/acuuid.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/acuuid.h) | A2 | 89 | `0906254bc08a08a291a66315fea6b197a57fe708` |
| [`source/include/amlcode.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/amlcode.h) | A2 | 480 | `c1fde40df7e10a6decf2decc82d5fbbbc181a90f` |
| [`source/include/amlresrc.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/amlresrc.h) | A2 | 762 | `4cbb50d328f430592115aef515dd674ef463f920` |
| [`source/include/platform/acenv.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/platform/acenv.h) | A2 | 387 | `dfd30ef451a473edf4e366fb82b221bcb9dcf10b` |
| [`source/include/platform/acenvex.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/platform/acenvex.h) | A2 | 52 | `bf22950bfdb0541acd14a2a09f262db74f4324e3` |
| [`source/include/platform/acgcc.h`](https://github.com/open-acpica/acpica/blob/20260930/source/include/platform/acgcc.h) | A2 | 97 | `bd107606a722c4626b10a900909912bb8152d211` |

## Alternative implementations considered

F1-116 additional donor: Linux `v2.6.18`,
[`sound/oss/maestro.c`](https://github.com/torvalds/linux/blob/v2.6.18/sound/oss/maestro.c),
legacy GPL version 2 or later grant, Alan Cox/Thomas Sailer/Zach Brown notices.
Adaptation and per-file origin are recorded in
[`maestro-NOTICES.txt`](../licenses/maestro-NOTICES.txt) and
[the Maestro transport decisions](f4-maestro.md). Linux v6.12 `ac97_patch.c`
was inspected as comparison only; no code from it is imported here.

These file headers were inspected individually. Their verdicts apply to the
listed file only; taking other framework/header code requires a new audit.
None is selected for transplantation in f1-102.

| Source and immutable coordinate | Header verdict and reason for comparison | Git blob SHA-1 |
| --- | --- | --- |
| [`freebsd/freebsd-src:sys/dev/uart/uart_dev_ns8250.c`](https://github.com/freebsd/freebsd-src/blob/release/14.2.0/sys/dev/uart/uart_dev_ns8250.c), `release/14.2.0` | BSD-2-Clause; compatible with retained notices. Compare bounded UART operations. | `ced3b44b57497a345eff91d3c1e2f124e9167980` |
| [`freebsd/freebsd-src:sys/dev/fdc/fdc.c`](https://github.com/freebsd/freebsd-src/blob/release/14.2.0/sys/dev/fdc/fdc.c), `release/14.2.0` | BSD-3-Clause; compatible with retained notices/non-endorsement. Compare FDC state machine. | `c51d36baaecbf7c23a08a9de2779a816489c71bb` |
| [`haiku/haiku:src/add-ons/kernel/bus_managers/ps2/ps2_standard_mouse.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/bus_managers/ps2/ps2_standard_mouse.cpp), `r1beta5` | MIT per file header; compatible with notice. Compare PS/2 protocol handling; C++/Haiku adapters are not selected. | `a43c7491aa6f5125e870b33c08ecd2c624cb0daf` |
| [`haiku/haiku:License.md`](https://github.com/haiku/haiku/blob/r1beta5/License.md), `r1beta5` | MIT license text; explicitly warns that other repository files have exceptions. | `e56b9308beaea7799619b268e153d0045d4a3e98` |
| [`NetBSD/src:sys/dev/isa/fd.c`](https://github.com/NetBSD/src/blob/12c634f788f1d67d0a7dc96fa94fba215f532dc7/sys/dev/isa/fd.c), `12c634f788f1d67d0a7dc96fa94fba215f532dc7` | NetBSD Foundation 2-clause + retained Berkeley 3-clause grants and FreeBSD contributor notices; compatible if all retained. No advertising clause in the inspected header. Compare FDC. | `e1d28801a8d720ab889d44f2a1fe68a898606faf` |
| [`alsa-project/alsa-firmware:ymfpci/hwmcode.c`](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/ymfpci/hwmcode.c), `v1.2.4` | Yamaha all rights reserved; no explicit per-file redistribution grant. Unresolved, NOT admitted for image distribution. | `499a6669748fedbcf8c3995a7017424381eb6d9b` |
| [`alsa-project/alsa-firmware:ymfpci/fw_writer.c`](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/ymfpci/fw_writer.c), `v1.2.4` | ISC-style permission + disclaimer; compatible writer, but does not license hwmcode.c. | `fbfa4c3b590a8d37380a88a23cf9a5b65a72f820` |
| [`alsa-project/alsa-firmware:COPYING`](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/COPYING), `v1.2.4` | GPL v2 text for package; does not resolve the Yamaha per-file rights exception. | `b74ec10193386f8d40ec03912da17e010fa6cc40` |

Yamaha research also checked the package's
[README license section](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/README),
which says firmware rights vary by loader/subdirectory. Its general GPL COPYING
file and the permissive firmware writer are insufficient evidence of Yamaha
microcode redistribution rights. A later directive must obtain and record the
specific applicable grant or leave DS-XG disabled with a clear missing-firmware
reason. Firmware bytes downloaded here are research-only, never image payloads.

## f1-123 add-in ATA sources

The f1-123 adaptations also use Linux v6.12 `pata_hpt3x2n.c`, blob
`5b1ecccf3c8388aaaf51768e79f71982e9a9ea93` (GPL-2.0-only). The Promise
and HighPoint donor blobs in the table above were fetched again through the
GitHub contents API and matched those pins. Complete donor files remain
research-only under `build/research/f1-123/`; no libata framework is linked.
Ciuki adapters replace PCI/MMIO/I/O, clocks, IRQs, locks, DMA and block services.
`ata_highpoint_tables.h` retains the donor timing values and drive blacklists.
All five original headers are in `docs/licenses/ata-addin-NOTICES.txt` and
every donor has a separate credit.

RAID refusal uses dmraid metadata constants: `pdc.h` blob
`f0baf9ec70c7d96b6ff6061d7b876c42832bb8f7`, `pdc.c` blob
`475bf6bb9a589afddbeb83b5306d31bb0c78e050`, `hpt37x.h` blob
`23e94607faa59df50202c9acae9be614f0a67c3f`, and `hpt45x.h` blob
`bcb315a997ad04e2cab0b7a96d52982540c51e52` from
[`Distrotech/dmraid`](https://github.com/Distrotech/dmraid/tree/master/lib/format/ataraid).
dmraid is GPL-2.0-or-later; the hpt37x format header retains its BSD-3-Clause
Søren Schmidt / Heinz Mauelshagen notice. The compatible FreeBSD signature
comes from `freebsd/freebsd-src` releng/14.2 `sys/geom/raid/md_promise.c`,
blob `da2ea3d7da4eb6a1bd5281c44039dfa14bcde75c` (BSD-2-Clause).
Those notices and the dmraid licence statement are retained in the same
notice file. There is no array assembly, repair or metadata write code.

## f1-108 additional ATA timing source

Linux v6.12 `drivers/ata/libata-pata-timings.c`, blob
`af341226cc64f672a41bd36b061f657cbee19373`, is GPL-2.0-or-later.
Only the UDMA 0–6 period table is adapted into `ata_via.c`; its Red Hat /
Jeff Garzik copyright, Vojtech Pavlik origin and SPDX notice are preserved
in `docs/licenses/ata-chipset-NOTICES.txt` and the credits. The five ATA
driver donors above retain their original headers alongside Ciuki changes.
Linux v6.12 `drivers/ata/libata-sff.c`, blob
`67f277e1c3bf317e5c5030cfabdb7f7d344429fe`, was additionally read for
native BAR/control and shared IRQ semantics; it is not an imported runtime
file. The research copies and byte-verified manifest stay in ignored
`build/research/f1-108/`.

## Behavioral and test references, not port sources

### f1-112 implementation use

`src/kernel/drivers/serial_protocol.c` adapts only the mouse wire equations
from Linux v6.12 `drivers/input/mouse/sermouse.c` (GPL-2.0-or-later,
Copyright (c) 1999-2001 Vojtech Pavlik), blob
`218c8432a13bc9addfe54a8818ee3701f7dd6795`. Its original notice and the
adaptation origin are retained in that file and
`docs/licenses/serial-mouse-NOTICE.txt`; the credits and image notices include
the adaptation. No imported `.c` file includes another `.c` file.
FreeBSD `release/14.2.0` `usr.sbin/moused/moused.c`, blob
`dcfd72099cb9a77711067df87eeb8fc52bd21cbe`, was checked for PnP ID/protocol
facts only. Microsoft `serial/serenum/enum.c` was fetched through the GitHub
read connector at the existing audited blob below; it remains research-only.
Donor research downloads are confined to `build/research/f1-112/`.
The new UART state model is original Ciuki code against TI TL16C550D;
SeaBIOS `rel-1.16.3` `src/serial.c` is an INT14 behavior reference only.
The firmware adapter additionally follows ACPI `_STA`/`_CRS` and IRQ
polarity/trigger/sharing descriptors; it never invokes `_SRS`.

### f1-111 implementation use

`drivers/com.c` is Ciuki code informed by the audited Linux v6.12 8250
detection and PCI layout algorithms above, with the retained serial.c authors
and Russell King attribution in its header. No upstream source/header is
compiled wholesale, and no foreign UART/TTY framework is imported. UART
register facts also use TI's [TL16C550D](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf),
[TL16C750 revision C](https://www.ti.com/lit/ds/symlink/tl16c750.pdf), and
Exar's [ST16C650A revision 5.0.3](https://www.maxlinear.com/ds/st16c650a_503_061610.pdf).
Those manuals are research references, not redistributed runtime assets.
The additional QEMU [PCI serial specification](https://www.qemu.org/docs/master/specs/pci-serial.html)
is used for its documented I/O layout only; no QEMU source is imported or run.
Public COM records and the SDK include are original MIT ABI definitions;
the implementation remains GPL-2.0-only. Local Resource Kit chapter 24's
communications-resource/VCOMM discussion is behavioral research only; no
Microsoft code or media is copied to tracked files.

### Existing census references

f1-114 adds original GPL-2.0-only `drivers/lpt.c`, `lpt_native.c` and
`vm/dosvm_lpt.c`, with original MIT public ABI/SDK declarations. No donor
source/header or notice-bearing fragment is imported. The National
Semiconductor PC87338/PC97338 (November 1998) register chapter, Intel 8237A
231466-005 Figure 6, Linux v6.12 `drivers/parport/parport_pc.c` and SeaBIOS
rel-1.16.3 `src/serial.c` are behavioral references. The attempted
`ieee1284_ops.c` and `src/std/bda.h` fetch failures are recorded in the
implementation report; their earlier census audit remains unchanged.
The local Windows 98 Resource Kit is read-only research. No vendor source
is compiled, so no new third-party credit or C-include symbol audit is due.

Microsoft's
[serial enumerator](https://github.com/microsoft/Windows-driver-samples/blob/main/serial/serenum/enum.c)
was read for the COM PnP sequence. The branch link is not an immutable pin;
blob `35ebe2ceb5e3995c2fde40a920dad4bd840561e0` identifies the inspected
bytes. Its file header contains copyright but no standalone license grant;
no sample code is selected for copying and no project-wide license is presumed.
The Microsoft/Hayes specification and Linux decoder remain the implementation
references. This NT sample does not establish Windows 98 V86 architecture.

SeaBIOS `src/std/bda.h` at `rel-1.16.3`, blob
`90d64a60da6723ec2c06c37d6a6e8e57c0036c17`, was read only to verify firmware
field positions. Its header has no grant. No SeaBIOS implementation is selected
for copying by this contract; protocol facts do not require a foreign BIOS port.

QEMU v9.2.0 files linked in the coverage contract were inspected for model names
and emulator limitations only. No QEMU file, header, license or source build is
added to the Ciuki runtime. No `-device help` command or VM was executed. These
references must not be presented as driver port sources or runtime evidence.

## f1-104 implementation boundary

`src/kernel/usb/hid_report.c`, `hid_input.c` and
`src/kernel/include/ciuki/hid.h` are original GPL-2.0-only implementations
of the USB-IF HID 1.11 and HUT 1.12 protocol facts cited in the coverage
contract. The existing project-owned `usb_hid.c` mouse projection now uses
that common parser. Linux v6.12 `hid-core.c` and `hid-input.c` listed above
were consulted for behavioral comparison; no Linux implementation, header,
dependency or notice-bearing fragment is transplanted. The new public
`ciuki/input_abi.h` is MIT, matching the existing public ABI's license.
Synthetic descriptor fixtures are project-owned GPL-2.0-only test data.
No Resource Kit text, media, vendor firmware or foreign driver binary is
included. Unsupported descriptor constructs are bounded failures, not a
claim of complete HID, XInput, vendor protocol or force-feedback support.

## f1-113 implementation boundary

The analog sampler, common joystick controls and virtual RC/BIOS implementation
are newly authored Ciuki code. No Linux or BIOS source is vendored, adapted
verbatim, compiled, or included as a C implementation file. Therefore there are
no new imported-file notices or credits entries. The v6.12 game-port paths in
the audit remain the intended pinned behavioral references; this sandbox could
not fetch those tags. Browsable upstream master versions of
[`ns558.c`](https://github.com/torvalds/linux/blob/master/drivers/input/gameport/ns558.c),
[`analog.c`](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/analog.c),
[`gameport.c`](https://github.com/torvalds/linux/blob/master/drivers/input/gameport/gameport.c)
and [`emu10k1-gp.c`](https://github.com/torvalds/linux/blob/master/drivers/input/gameport/emu10k1-gp.c)
corroborated trigger/decay, timeout, measured timing and Creative BAR0 behavior.
The extra PCI ID table is not a donor import. The
[PCI-SIG Code and ID Assignment specification, revision 1.11, class 09h note 1](https://pcisig.com/sites/default/files/files/PCI_Code-ID_r_1_11__v24_Jan_2019.pdf)
defines programming interface 10h I/O BARs as standard legacy game ports;
interface 00h alone proves no register layout. Integrated audio legacy decode
requires its parent driver and is not enabled by a name match here.
