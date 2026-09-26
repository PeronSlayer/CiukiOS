# Hardware profiles and driver directory

`\DRIVERS` is the canonical user-visible location. The built-in keyboard,
mouse, BIOS disk and VBE backends remain kernel/shell components. External
drivers are separate programs; putting a `.SYS`, VxD or WDM file into a
directory does not make it a supported driver ABI.

## Detector contract

Build: `nasm -f bin -o hwdetect.com src/com/hwdetect.asm`.

`HWDETECT` reports the profile to standard output. `HWDETECT /Q /SAVE` writes
`\DRIVERS\ACTIVE.NEW`, checks the full write and successful close, then
publishes it as `\DRIVERS\ACTIVE.CFG`. It does not create the parent folder:
the installer/build must provide it. No arguments require keyboard input.
Exit codes are 0 for completed detection/publication, 1 for invalid arguments,
5 for publication failure. Missing PCI, VBE or SMBIOS support is a normal
generic profile, not a program error.

Boot integration must run the detector once on **each boot**, before startup
audio, and consume `ACTIVE.CFG` only after child exit code 0 and normal child
termination. Also require `SCHEMA=1` and the final `END=1` line. On a failed
run, discard any stale cached profile and retain safe built-in defaults. A
report from a previous machine must never be treated as current detection.

The format is ASCII, CRLF, less than 512 bytes. Example for an E500:

```ini
SCHEMA=1
MODEL=COMPAQ_E500
INPUT=I8042_BIOS
VIDEO=VBE
AUDIO=UNSUPPORTED
AUDIO_FALLBACK=SB_NATIVE
AUDIO_PCI=125D:1978
AUDIO_SUBSYS=0E11:B112
VIDEO_PCI=1002:4C4D
PCI_BIOS=1
END=1
```

`MODEL=COMPAQ_E500` requires a matching SMBIOS product string. With only an
ESS1978 controller and Compaq subsystem vendor, use `COMPAQ_ES1978`: the same
audio subsystem exists in other Armada models. `THINKPAD_T23` likewise requires
the product name; otherwise the model is `GENERIC_PC`. Profiles never infer
specific hardware quirks from a marketing name alone.

`INPUT=I8042_BIOS` selects the native standard path and its BIOS fallback.
`VIDEO=VBE` means VBE information is available; the existing video selector must
still validate individual modes, memory model, pitch and framebuffer access.
It is not a promise that a particular resolution is valid. `VIDEO=VGA` selects
the existing fallback.

`AUDIO=ICH_AC97` matches the native player's exact Intel device allowlist.
`VSBHDA` matches the shipped VSBHDA device tables or PCI HDA class, selecting
the existing transient VSBHDA launcher. `SB_NATIVE` means no supported PCI
controller was detected and only the existing native legacy probe should run.
`UNSUPPORTED` means a PCI audio controller was found without a matching backend;
the native legacy probe may still be tried, but the controller must not be
programmed as Intel ICH or Allegro merely because it uses an AC97 codec.
`FFFF:FFFF` denotes an unavailable PCI identity.

## Detection boundaries

- PCI BIOS B101 presence, B103 class enumeration and B10A config reads only.
  At most 16 functions per audio class; the first supported native Intel device
  wins over VSBHDA, which wins over unsupported audio. No PCI command writes,
  BAR probing, direct CF8/CFC accesses, device reset or bus-master changes.
- VBE function 4F00 information only; the detector never changes video mode.
- SMBIOS2's legacy `_DMI_` entry point in F0000..FFFFF, with checksum,
  structure-count, header, string and table-length checks. Only bounded tables
  wholly below 1 MiB are read. SMBIOS3/high-memory tables are not mapped.
- Hardware BIOS calls preserve all caller GPRs, segment registers and flags.
  The program uses its own 2 KiB stack and no resident allocation.

The report selects an existing backend. It is not proof that a physical machine
has passed an end-to-end hardware test.

## Packaging

`assets/drivers/payloads.json` maps redistributable source assets to DOS 8.3
paths. Include the **matching source ZIP and COPYING** alongside CuteMouse.
Build/integration also installs `HWDETECT.COM` in `\DRIVERS` and keeps the
existing `\SYSTEM\DRIVERS` and `\SBEMU` launcher paths working. Internal
driver aliases should point to/copy identical built bytes, not independently
versioned replacements. The full installer manifest must cover every newly
packaged payload so the installed HDD has the same driver set as the live disk.

The kernel now releases native PS/2 IRQ/BIOS callback ownership after failed
initialization. At boot `INPUTINI.COM` first checks INT33; successful native
input is retained. Otherwise it executes CuteMouse with `/P /W /B`, consumes
the child status and verifies the installed interface. It does not stack a
replacement above a functioning native driver. This fallback is a bounded
BIOS PS/2 path, not a USB host driver or proof of physical E500 operation.

The shell currently uses fresh `AUDIO=ICH_AC97` or `AUDIO=SB_NATIVE` for the
native boot PCM player. Other PCI families remain on their existing
application-specific transient launchers; inventory availability alone does
not create a new startup sound backend. `VIDEO=VGA` selects the existing
recovery renderer, while VBE mode acceptance remains with the video selector.

Reference for SMBIOS entry-point and table validation:
[DMTF DSP0134 2.6.1](https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_2.6.1.pdf).
