# Hardware ID database coverage

This is the historical f2-43 snapshot. For the current f2-123 generator,
class scope, display-family coverage and table sizes, see
[the current validation record](../validation/f2-123-hardware-names.md).

Generated from the owner's read-only Ultimate Windows Driver Pack INF
collection on 2026-10-13. `assets/hwids/README.md` records provenance and the
generation command. The database contains 1,155 PCI pairs and 218 USB pairs
(75,014 combined JSON bytes); labels are identification hints, not driver
support claims.

## Baseline coverage

For repeatable counts, the baseline sample is the 20 PCI pairs already named
as concrete candidates by the Device Manager's pre-database fallback table,
plus the seven additional Intel ICH3-M function IDs (`2480`, `2482`–`2484`,
`2486`, `248a`, `248c`) and the T23/E500 identities called out in the hardware
baseline and device ownership contract. Duplicate pairs count once. This is a
coverage sample, not an exhaustive enumeration of every vendor family in the
baseline (many families do not specify a single PCI model).

**17 of 27 sample IDs (63%) are present.**

| PCI ID | Baseline device | In database | Selected INF label when present |
| --- | --- |:---:| --- |
| `8086:1237` | 440FX host bridge / QEMU baseline | no | — |
| `8086:7190` | 440BX host bridge | no | — |
| `8086:3575` | 830MP host bridge, T23 | yes | Intel(R) 82830 Processor to I/O Controller - 3575 |
| `8086:2480` | ICH3-M LPC interface | yes | Intel(R) 82801CA LPC Interface Controller - 2480 |
| `8086:2482` | ICH3-M USB controller | yes | Intel(R) 82801CA/CAM USB Universal Host Controller - 2482 |
| `8086:2483` | ICH3-M SMBus controller | yes | Intel(R) 82801CA/CAM SMBus Controller - 2483 |
| `8086:2484` | ICH3-M USB controller | yes | Intel(R) 82801CA/CAM USB Universal Host Controller - 2484 |
| `8086:2486` | ICH3-M modem function | yes | GTW Modem |
| `8086:248a` | ICH3-M IDE controller | yes | Intel(R) 82801CAM Ultra ATA Storage Controller - 248A |
| `8086:248c` | ICH3-M LPC interface | yes | Intel(R) 82801CAM LPC Interface Controller - 248C |
| `8086:7000` | PIIX ISA bridge | no | — |
| `8086:7010` | PIIX IDE | no | — |
| `8086:7110` | PIIX4 ISA bridge | no | — |
| `8086:7111` | PIIX4 IDE | no | — |
| `8086:2415` | ICH AC'97 / QEMU audio candidate | yes | Realtek AC97 Audio |
| `8086:2485` | ICH3-M AC'97, T23 | yes | Realtek AC97 Audio |
| `8086:1229` | Intel PRO/100 family | yes | Intel(R) PRO/10+ PCI Adapter |
| `8086:1031` | ICH3-M PRO/100 VE, T23 | yes | iSeries 2892 10/100 Ethernet Port |
| `5333:8c2e` | S3 SuperSavage, T23 | no | — |
| `1002:4c4d` | ATI Rage Mobility, E500 | no | — |
| `125d:1978` | ESS Maestro-2E, E500 | yes | ESS Maestro2E PCI AudioDrive (WDM) |
| `1013:00b8` | Cirrus GD5446 desktop candidate | no | — |
| `1234:1111` | QEMU Bochs VBE display | no | — |
| `10ec:8139` | Realtek RTL8139 network candidate | yes | Realtek RTL8139C+ Fast Ethernet NIC |
| `10b7:9055` | 3Com 3C905B network candidate | yes | 3Com EtherLink 10/100 PCI TX NIC (3C905B-TX) |
| `1106:3099` | VIA KT266 host bridge | yes | VIA Standard CPU to PCI Bridge |
| `1106:3074` | VIA VT8233 ISA bridge | yes | VIA Standard PCI to ISA Bridge |

The T23 PRO/100 VE ID `8086:1031` is corroborated by Intel's I/O Control Hub
EEPROM map application note, which associates `1031h` with 82562EZ PLC / PRO/100
VE. The repository's hardware contract still requires fresh PCI/subsystem/PHY
inventory before freezing an F6 allowlist. T23 ICH3-M audio `8086:2485` is the
recorded device in `f4-audio-media-contract.md`. The T23 graphics ID and E500
IDs are retained as owner-recorded identities in
`device-firmware-ownership.md`.

Some shortest labels are clearly generic or inconsistent with the PCI vendor
(for example `8086:2485` resolves to `Realtek AC97 Audio`). Keep the source
label as required for provenance, but treat it as untrusted display metadata;
the app's existing small table remains the fallback when no pack entry exists.
This database never selects a driver.

## Ten most common PCI vendors

Counts are distinct PCI vendor/device pairs after deduplication, grouped by
vendor ID. They show collection emphasis, not market share or runtime support.

| Rank | Vendor ID | Vendor | Distinct device IDs |
|---:| --- | --- | ---:|
| 1 | `8086` | Intel | 438 |
| 2 | `10de` | NVIDIA | 243 |
| 3 | `1106` | VIA | 167 |
| 4 | `14e4` | Broadcom | 62 |
| 5 | `11c1` | Agere/Lucent | 29 |
| 6 | `9005` | Adaptec | 27 |
| 7 | `10b7` | 3Com | 21 |
| 8 | `1002` | ATI/AMD | 17 |
| 9 | `11ab` | Marvell | 17 |
| 10 | `1217` | O2Micro | 17 |

Implications for future driver planning: Intel, NVIDIA and VIA dominate this
pack; 3Com and Intel provide meaningful NIC coverage; ATI has modest display
coverage; the ten-vendor list alone says little about audio, where the pack
has only a small number of device pairs. Prioritize from runtime target needs
and hardware evidence, not these counts.

## Research and implementation decision

Microsoft documents that INF model rows map a device description token to a
hardware ID and that `%strkey%` values are resolved from `[Strings]` (with
locale-decorated strings selected before the undecorated section):
[INF Models Section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-models-section),
[INF Strings Section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-strings-section),
[General Syntax Rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/general-syntax-rules-for-inf-files).
The parser therefore gathers sectioned entries, resolves string tokens, and
handles UTF-16 and legacy Windows text encodings. Malformed/unresolved entries
are skipped. The deterministic output keeps only PCI/USB ID pairs and names;
the JSON is bounded well below 400 KiB. At runtime the PCI JSON is loaded on
the first Device Manager name lookup; its label precedes the old product table,
then the existing class-name fallback. The USB database is installed for
future USB enumeration; current call 76 records do not expose USB IDs.
