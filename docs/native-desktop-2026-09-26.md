# Native desktop repair, 26 September 2026

This increment fixes the existing cooperative Doom preview controls, makes
removable-media browsing graphical, extends wallpaper installation and adds
licensed system event sounds. **It does not complete original DOS graphics
applications in windows.** The user's requirement is to execute the original
DOS binaries with their audio and devices. Source ports do not satisfy it and
must not be used as evidence for it.

## Changes

- Doom preview now maps Ctrl and Space to the engine's actual fire/use codes.
  Keyboard qualification reads the demo recorded by the real running game:
  individual keys, right Ctrl, simultaneous fire/use, movement/fire and releases.
  The `-nogui` default also prevents its DOS error path trying Unix `zenity`.
- CD, floppy and BIOS-exposed disks open native Files. Navigation, text preview,
  import, source-read-only notices, missing-media and destination-exists errors
  remain graphical. Sources are never written. See
  [media capabilities and limits](native-removable-media-2026-09-26.md).
- Wallpaper supports 99 tiles, PNG/BMP conversion, append-only installation and
  Refresh. Malformed files preserve the previous background. Public builds
  contain three original CC0 patterns; the owner's Windows artwork remains an
  explicit personal-build option. See [adding wallpapers](wallpaper-import-2026-09-26.md).
- System event audio uses **Kenney Interface Sounds 1.0**, CC0, downloaded from
  [the creator](https://kenney.nl/assets/interface-sounds). Original archive,
  audio, license and hashes are in `assets/sounds/kenney-interface/`; credits
  and license are included in `SYSTEM\SOUNDS`.
- Error, information, warning and confirmation PCM files are separate from the
  original CiukiOS startup melody. Files errors, deletion confirmation and media
  import trigger the corresponding events. Sound settings preview and save
  preferences without leaving the desktop.
- `SFX.DRV` starts native ICH AC97 or supported Sound Blaster DMA and returns to
  the desktop; foreground polling observes completion. It installs no interrupt
  handler. Playback and buffers are released before a DOS child; Safe/MUTE boot
  choices skip automatic device initialization. Explicit Play can initialize
  audio. Unavailable devices and invalid sound files have visible status text.
  See [Sound Blaster ownership and emulator limits](native-sb-events-2026-09-26.md).
- Shell history and graphical-console glyph scratch now live in separate
  allocated memory, retaining the original COM size guard. The shared VBE
  validator accounts for the additional 512-byte glyph buffer. Conventional
  memory is checked by actual allocation metadata in the console test.

## Packaged image and evidence

The integrated build is
`build/full/native-desktop-2026-09-26/final/ciukios-native-desktop.img`.
SHA256: `08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`.
It is a 128 MiB bootable disk image, not an ISO and not a newly burned disc.
The image is a personal build containing the existing user-provided software
and wallpapers; the Kenney sound library has its own independently recorded
CC0 license.

`manifest.json` beside the image records packaged binaries; `source-freeze.json`
records their sources. `SHELL.COM` is 60,832 bytes, `SFX.DRV` 9,024 bytes and
`MEDIA.DRV` 11,773 bytes. Kernel, COMMAND.COM, DOSWIN.DRV and BOOTSND.COM are
byte-identical to the preceding selected build. This fact does not substitute
for integration checks of the changed shell.

Tests use copies of the image, actual QEMU keyboard/mouse events, observed
framebuffer/PCM data and independently read file contents. They do not inject
guest RAM events or modify physical media. The final directory contains the
reports from the integrated image; their [published JSON snapshot](validation/2026-09-26/README.md)
is retained in the repository. Earlier candidate folders include failures
and must not be treated as passing release evidence. QEMU uses a Pentium III CPU
model and 128 MiB RAM; these checks do not qualify physical T23/E500 behavior or
prove a hardware frame rate.

## Remaining original-DOS work

The current window runtime runs a restricted BIOS-text foreground DOS process.
The graphics previews use a cooperative API. It lacks a V86 monitor that owns
direct VGA memory/ports, a compatible protected-mode host renderer and shared
DPMI/audio-device ownership. Arbitrary unmodified DOS games can therefore still
change physical video or bypass its keyboard path. Their complete windowed
execution, audio and hardware acceleration remain unimplemented.

The existing [native DOS architecture](dos-window-architecture-2026-09-26.md)
documents the inspected Jemm and HDPMI sources. The following three work packages
can start in parallel against an agreed ABI; final integration depends on the
first package. They are remaining implementation work, not completed features.

1. **Session monitor.** Implement an opt-in V86 session host using the pinned
   Jemm infrastructure. Define lifecycle, CPU context, virtual interrupts,
   address mappings, input and device ownership. Execute an unmodified COM
   child with direct B800 writes and file I/O; demonstrate host responsiveness
   and normal cleanup. Preserve the existing boot and fullscreen paths.
2. **Virtual VGA and protected-mode presentation.** Implement monitor-owned
   B800/A000 surfaces, text/mode13h/required planar modes, palette/register traps
   and focused raw keyboard events. Adapt framebuffer transfer to a protected
   host service; do not run the current real-mode CR0 transition inside V86.
   Demonstrate original binary pixels in overlapping windows at actual supported
   resolutions, including movement, held Ctrl/Space and focus loss.
3. **DPMI and audio integration.** Attach the existing HDPMI/VSBHDA paths to the
   same monitor instead of competing interrupt/I/O owners. Run the original
   Doom/Wolf binaries, capture genuine audio, measure visible frame delivery and
   verify resource restoration. Qualify real T23/E500 hardware separately;
   Windows 3.1 is its own compatibility gate. Keep binary/version/failure evidence
   and do not replace this acceptance with source-port tests.
