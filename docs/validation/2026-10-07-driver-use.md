# Automatic display mode and actual driver use — 2026-10-07

## Physical evidence and the repair

The connected T23 disk was acquired read-only at 18:44:30 UTC, serial
`H551120730`, disk sequence 56. Its acquisition manifest is
`build/full/t23-vbe-fix/physical-logs-20261007-1844/manifest.json`.
The selected devices are S3 `5333:8C2E` and Intel AC97 `8086:2485`.
`LOADDRV.LOG` reports S3VBEFIX installation success, but the old image has
neither an active-mode capture nor a native startup-audio log. Its `1024`
display configuration is a requested profile, not evidence of the active
resolution or depth. Its DOSVM log contains session lifecycle records, not
a DOOM launch. The physical SHELL and SFX binaries match the previous build.

The full image now defaults to AUTO, ranks checked modes within valid monitor
EDID dimensions by area and then color depth, and tries other direct-color
modes if firmware rejects the first startup mode. Without valid preferred
EDID, automatic selection is bounded to 1024×768. Interactive mode previews
retain their exact requested mode. Driver installation additionally records
the INT10 vector before and after VIDEO installation. A changed vector is
evidence of hook installation; it does not establish a hook's effect on an
individual BIOS call.

`SYSTEM/VIDEO/DISPLAY.LOG` records active geometry/depth, 4F03 readback,
failure/retry information, VBE/EDID blocks and renderer-path use after the
first paint. Bank/LFB bits record entry into the validated renderer path;
protected-session row counts record completed transfers.
`SYSTEM/AUDIO.LOG` records the native codec, power/mixer state and DMA
progress. Startup audio streams through the existing 64 KiB ring, removing
the additional whole-track conventional-memory allocation. Desktop volume
access temporarily enables PCI I/O decode and uses the codec semaphore;
it preserves bus-master enable and rejects invalid reads instead of showing
a false mute indication.

Research, register contracts and remaining V86/S3 limitations are recorded
in [the video design](../design/t23-hardware-rendering.md),
[startup audio design](../design/t23-startup-audio.md), and
[mixer design](../design/t23-mixer-topbar.md). No native S3 blitter or new
S3-specific V86 LFB path is claimed by this change.

## CPU and host checks

| Check | Result |
| --- | --- |
| Actual assembled AUTO/startup-retry instructions | 19 checks passed |
| Actual assembled VIDEO hook logger and caller preservation | 4 checks passed |
| VBE 3 layout, active pitch, readback and V86 guards | 13 groups passed |
| Actual assembled startup streaming instructions | 20 cases passed |
| CVB1 parser, invalid descriptors and EDID | 4 tests passed |
| Compiled production desktop mixer helpers against port/PCI mocks | 13 scenarios passed |

The retry checks include malformed mode IDs, unmodified firmware ROM lists,
15/16/24/32-bpp ranking, rejected mode sets/readbacks/allocations, no repeated
failed candidates, bounded all-fail behavior and exact interactive requests.
Streaming checks include queue exhaustion, concurrent consumption during
refill, CIV/tick wrap, partial final descriptors, stop/error cleanup and
complete 245,760- and 524,288-byte output with one 64 KiB allocation.

## Linux runtime evidence

Full HDD and CD builds and QEMU runs were sequential in systemd user scopes
with `MemoryMax=3G`, `MemorySwapMax=1G` and one CPU quota. Builds used one
job. The standalone floppy profile was not built or tested.

The pristine full-HDD gate `boot-hardware-ciuk1-final` tested image SHA-256
`a73fc89ab2b6904aa44c772cb4aea30dcdd0f8b837a32489a4201b561984c7ee`
with 128 MiB QEMU RAM, emulated 1280×800 EDID and AC97. It passed:

- automatic 1280×800, 32-bit direct color and exact active-mode readback;
- actual completed protected row transfers and successful keyboard return
  from a DOS session;
- all three installed Ciuk wallpapers matching the owner's source pixels;
- Ciuk1/Fill at startup and after DOS, with 36 exact RGB samples on each
  completed desktop screenshot;
- native initialization/start/EOF records, all 245,760 startup bytes fetched
  by DMA, verified ownership release, and a nonconstant captured waveform.

Additional pristine HDD runs passed 1024×768×32 with 1024×768 EDID and with
EDID disabled; the latter verified the no-EDID bound. The music-driver gate
passed WAV, MP3, Ogg Vorbis and FLAC, including pause/resume, volume, seek,
EOF, previous-track navigation, close/reopen and stop. All four formats
advanced beyond 16,384 stereo frames; the captured audio contained changing
post-boot output. Reports and captures are under
`build/full/t23-vbe-fix/`.

The final CD-only runtime gate also passed, with no HDD device attached:
freshly assembled SHELL and installed wallpaper assets matched the CD
partition, the desktop and startup About opened, and the completed Ciuk1
Fill screenshot matched source RGB samples. ISO SHA-256:
`6e7dec7133f58fea3481adea5c4d7ac8d8de59cc2023c5fc9344db9344a63612`.
Its report is `driver-use-final-cd-runtime/report.json` in the same directory.

These are emulator results. Physical T23 resolution, sound at its speakers,
rendering performance and DOOM execution require a fresh hardware boot and
the resulting disk logs. No Windows launcher execution on Windows is claimed.
