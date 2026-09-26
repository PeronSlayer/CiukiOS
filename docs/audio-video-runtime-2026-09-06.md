# Audio launchers and VGA Setup, 2026-09-06

The user's running QEMU process exposed SB16 at A220/I7/D1/H5 plus AdLib,
with KVM, a Pentium III CPU, 256 MiB RAM and standard VGA. Its serial log
confirmed that Doom, Doom Vanille, Wolf3D and Windows all returned immediately
after `SB found - probably VSBHDA already installed`. The physical T23 also
showed a blank display when opening VGA Setup. A manual `sound test` on the
T23 returned to DOS without an audible sound or a readable diagnostic.

Evidence directory: `build/full/audio-video-runtime-2026-09-06/`.
`user-qemu-visual.log` preserves the user's original QEMU log. Test VMs use
separate writable image copies; the user's running VM is not used for tests.

## Reproduced launcher failure

The released launchers unconditionally ran the AC97/PCI emulator, even when
an ISA Sound Blaster was present. Upstream VSBHDA detects that card and returns
zero **before reaching the patched synchronous /RUN path**. Consequently the
wrapper's cleanup message did not establish that the application ran.

This is consistent with the [upstream detection/early return code](https://raw.githubusercontent.com/Baron-von-Riedesel/VSBHDA/75fa4bbfea70cbcc0c40d1212f04952ff8abbf16/src/MAIN.C)
and [VSBHDA's documented detection message](https://raw.githubusercontent.com/Baron-von-Riedesel/VSBHDA/master/vsbhda.txt).
`before-sb16/` reproduces all four failures using the previously installed
release image, with no replacement kernel or shell.

The launchers now query the DSP version with bounded polling. With the native
Sound Blaster, they run the original engine directly under the temporary DPMI
host, preserving command arguments. Without that card, the existing VSBHDA
path remains in use. The native MZ launchers have their own initialized FCBs.
Wolf3D now checks the child's exit code as well as EXEC's carry flag.

The first extended native test also exposed a Doom Vanille freeze after a
shot: IRQs were pending while the client ran with IF clear and CR4.PVI clear.
`wolf-van-sb16/` retains the failed run, screenshot, RAM and CPU/PIC registers.
The native Doom Vanille path now saves/enables/restores PVI, using the same
routine as classic Doom. VSBHDA's [SETPVI/RESPVI documentation](https://raw.githubusercontent.com/Baron-von-Riedesel/VSBHDA/master/vsbhda.txt)
describes this DOS/4GW interrupt virtualization mechanism. The rerun in
`wolf-van-sb16-pvi/` completed gameplay, movement, shooting, audio and exit for
both games.

## Startup sound and visible failure

The original melody now also has a native Sound Blaster path, using the same
notes converted to unsigned mono PCM at 8 kHz. The non-resident SBSTART player
copies the whole sample into a buffer aligned to a physical 64 KiB boundary,
then releases its IRQ ownership and stops DMA before returning. Its firmware
and completion waits are bounded. The shared SB16 IRQ restoration code now
reads the saved vector number from CS after changing DS to the old handler's
segment.

Quiet startup remains silent when playback is unavailable. Manual testing
reports the failing step (device, file, PCI enable, codec, format, timer or
DMA), and retains a failure message until a key is pressed. Failed PCM
playback no longer substitutes PIT beeps or returns success.

`final-sound-ac97/` and `final-sound-sb16/` capture both automatic startup and
manual playback from the final QEMU image. `sound-no-card-final/` and
`sound-no-pcm/` exercise actual missing hardware/file paths, check visible
VGA text, require silence including the PC-speaker output, and execute a COM
program after returning to DOS. These recordings establish digital output
in QEMU, **not sound from the physical T23 speakers**.

## VGA Setup

Opening the menu no longer performs DDC or enumerates all VBE modes. It reads
the saved preference without firmware discovery; AUTO selects the 800x600
preview option. Only an explicitly requested preview probes that mode.
Optional serial diagnostics use a bounded UART check instead of waiting on
BIOS serial-transmit timeouts for a disconnected cable.

`vga-before/` reproduces a pre-menu hang using a test-only BIOS interceptor
that stalls discovery. `vga-after/` opens the visible menu with that same
fault present, exits and runs a COM. `vga-reject/` verifies a refused preview
returns to the menu. `vga-auto/` checks the real 800x600 preview, timeout,
unchanged settings after cancellation, confirmation, complete directory
listings and COMMAND.COM execution. This proves the guarded paths in QEMU;
the precise physical T23 firmware failure has not been measured.

## Release and qualification

The final ISO is `release/CiukiOS-audio-video.iso`, 149379072 bytes, SHA256
`5246c9d62e2573ee9fca98958414cd7732b2e74f322735d14c83cec77a0c222c`.
The matching raw source is `release/source.img`, SHA256
`1ef31a3da3fcc8e8bdcddcba110b01762e4ad158aaf74215ac335d4b2e3c7fd3`.
The separate normal QEMU image is `release/qemu-final.img`.

`release-payload-diff.json` compares every file against the last burned ISO:
1408 old files, 1409 new files. Kernel, shell, setup and application engines
remain byte-identical. Launcher/video/sound binaries change, and SBSTART is
added. HDPMI/VSBHDA helpers are rebuilt by the existing build pipeline; three
have timestamp-only differences, while VSBHDA16 has additional binary
differences and is included in the final Windows runtime qualification.

The normal QEMU image is installed at `build/full/ciukios-full.img`. The old
image was preserved by rename as
`qemu-in-use-before-20260906T210013Z.img`; an already open QEMU process keeps
its old file descriptor. The new image is used on the next launch with
`bash scripts/qemu_run_full.sh --no-build`. Its SHA256 is
`0edecb630dc7cdfba4f6d2a5a26281727e68e6eecd866e120df9bd08120da856`.
Both public QEMU launch scripts retain SB16/AdLib as their default and also
accept `QEMU_AUDIO_DEVICES=ac97` for the PCI path. The dry-run command checks
are recorded separately from actual guest runtime tests.

`final-install/` installs from the final ISO, compares every installed sector
with its source, observes a guest hardware RESET from Restart, and cold-boots
the HDD without a CD. Read-only fsck checks pass on the source partition, the
installed partition and the separate QEMU image.

| Final artifact / device / RAM | Actual runtime checks | Evidence |
| --- | --- | --- |
| QEMU image, SB16/AdLib, 256 MiB | Two Windows sessions, Calculator 2+3=5, resize/repaint, WAV and MIDI, DOS return | `final-qemu-windows-sb16/` |
| QEMU image, SB16/AdLib, 256 MiB | Classic Doom title, menu input, episode/skill, gameplay, movement, shooting, quit and COM | `final-qemu-doom-sb16/` |
| QEMU image, SB16/AdLib, 256 MiB | Wolf3D and Doom Vanille gameplay, movement, shooting, audio, quit and COM | `final-qemu-games-sb16/` |
| Installed HDD, AC97, 256 MiB | Two Windows sessions, Calculator, resize/repaint, WAV/MIDI and DOS return | `final-hdd-windows-ac97/` |
| Installed HDD, AC97, 256 MiB | Classic Doom menu/gameplay/audio/quit and COM; 38675 kernel code bytes checked | `final-hdd-doom-ac97-isolated/` |
| Installed HDD, AC97, 256 MiB | Wolf3D and Doom Vanille gameplay/audio/quit and COM; kernel code checked | `final-hdd-games-ac97/` |
| QEMU image, SB16/AdLib, 256 MiB | Same boot: Wolf3D, Doom Vanille, Windows, return to DOS, Caps Lock twice and COM; gameplay audio captured | `final-sequence-sb16-rerun/` |
| Installed HDD, AC97, 256 MiB | Same boot: Wolf3D, Doom Vanille, Windows, return to DOS, Caps Lock twice and COM; gameplay audio captured | `final-sequence-ac97-rerun/` |

Both final sequence processes exited with status 0. These runs use the same
release images as the earlier qualifications, with no application or driver
substitutions. They verify that successive temporary audio/DPMI sessions leave
Windows launch and subsequent DOS input/execution working in one guest boot.

Results and artifact identities are recorded in `release-manifest.json`.
The parallel classic-Doom timeout below remains an unresolved observation;
the isolated pass does not erase it.

## Retained unsuccessful runs

- `build-cd-first.log`: the packaging verifier correctly rejected the new
  SBSTART file until it was included in the expected generated payload. The
  verifier remains enabled; `build-cd-final.log` completed successfully.
- `wolf-van-sb16/`: the real native Doom Vanille interrupt freeze described
  above. This failed run is not counted as a pass.
- `sound-no-card/`: the first harness assumed QEMU creates a WAV file even
  without any sound device. The revised test attaches the PC speaker too,
  allowing it to detect unwanted beeps, and passes in a separate directory.
- `final-hdd-doom-ac97/`: with three qualification VMs running concurrently,
  classic Doom completed its menu/gameplay checks but exceeded the 60-second
  wait for exit. The CPU snapshot was inside SeaBIOS polling IDE; QEMU also
  exceeded the harness's 10-second process-close wait, then terminated. The
  precise cause is not established. `registers.log`, `pic.log`, screenshots
  and RAM are retained. The unchanged image passed a separate isolated run
  in `final-hdd-doom-ac97-isolated/`, including exit, subsequent DOS commands
  and the kernel integrity check. No timeout was increased to obtain that pass.
- `final-sequence-sb16/` and `final-sequence-ac97/`: both games completed and
  Windows opened, but the test failed to recognize the small Program Manager
  title in its screenshot. These runs did not reach the subsequent Windows
  exit or DOS checks. Applying the existing Windows qualification's contrast
  threshold and 3x enlargement recognized the same original screenshots.
  The revised test then completed fresh runs in the two `-rerun/` directories,
  including the previously unreached checks; the guest images were unchanged.

The VSCode session's 8,000,000,000-byte memory cap remains active. No physical
HDD has been written by this work. On the subsequent explicit burn request,
the final ISO was written to the CD-RW in `/dev/sr0`, after quick blanking,
at a requested 4x speed. cdrskin completed with exit status 0 and reported
149379072 bytes read and written; ejection was requested. No optical
readback verification was performed, as requested by the user. The burn
log and result are `burn-20260906T212653Z.log` and
`burn-20260906T212653Z.json` in the evidence directory.
