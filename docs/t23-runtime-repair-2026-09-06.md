# Runtime recovery after the T23 report

The physical report was: the desktop appears, applications do not start, DOS
accepts characters but does not execute DIR, then reports SHELL.COM missing.
That complete physical failure has not been reproduced on the workstation.
Passing emulator tests does not establish that this T23 fault is resolved.

## Changes

- GRUB still reads and decompresses the smaller CD image. Normal runtime RAM
  transfers now use BIOS INT 15h/87h (`map --memdisk-raw=0`) rather than GRUB's
  direct CR0/GDT/A20 mover. The direct mover remains an explicit alternate menu
  entry for BIOS implementations which cannot provide the memory-copy service.
  Neither path requires MEMDISK to decompress the image.
- The menu includes Live, Setup, safe graphics, DOS console, alternate disk
  access, and uncompressed recovery. Recovery also uses GRUB's BIOS transfer
  path. Physical disks are shifted once after clearing previous mappings.
  The RAM image remains hd0; setup's physical destination remains hd1.
- DOS startup consumes a four-byte `DOS!` request from the live RAM image and
  resets it to `LIVE` before entering the text console. EXIT opens the desktop.
- The splash timer held its initial tick in AX, but the next BIOS INT 1Ah call
  overwrites AH. It now holds that tick in SI and preserves CX. The completed
  progress bar stays visible for 36 ticks; together with progress this leaves
  the original Ciuki photo visible for about three seconds in the measured VM.
- Startup AC97 playback preserves the firmware's EAPD polarity and ADC power
  policy. It wakes and waits for DAC, mixer and reference, without requiring
  the unused ADC. Boot does not reset away the codec power policy. Extended
  status retains unrelated bits while selecting fixed-rate analogue playback.
  Windows/game VSBHDA and HDPMI payloads are reused unchanged.
- A successful explicit VGASETUP choice clears the shell's safe-video override.
  Previously safe boot could save a resolution and still ignore it on return.
  The shell records the child's exit status for the graphical settings result.
- Loader diagnostics retain EXEC's AX across screen restoration. Only DOS
  error 2 says that SHELL.COM is missing; other failures say execution failed
  and show the DOS error in hexadecimal. An unexpected shell exit remains
  distinct. This improves diagnosis; it is not evidence of the original cause.

The source of the GRUB mover is the pinned 2020-08-09 release, cached locally;
the [upstream assembly](https://github.com/chenall/grub4dos/blob/0.4.6a/stage2/asm.S)
contains both the direct mover and `int15_87` path. Its own comments acknowledge
that some BIOS movers are faulty, which is why the direct-access choice remains.
The AC97 power sequence follows the actual reused VSBHDA ICH source in this
workspace, including its tested EAPD/ADC preservation.

## Acceptance and rejected candidates

Evidence is under `build/full/t23-runtime-repair-2026-09-06/`. `artifact-final.json`
identifies the final ISO and payload hashes. Logs with `final-` names refer to
the final ISO; other logs include investigative candidates and failures.

The new machine-code tests run the assembled kernel timer at low, high and
wrapping BIOS tick values, and run the assembled BOOTSND codec routine against
ADC-off/EAPD, delayed/absent playback readiness and missing-codec states. The
PS/2 tests exercise interleaved keyboard/AUX bytes and failed acknowledgements.

The new ISO runtime test checks actual DIR entries, copies COM/MZ/COMMAND files,
executes the copies (including nested EXEC), checks deletion effects, changes
640/800/1024 resolution in both DOS and the desktop, checks cursor borders and
confirms the physical test disk is unchanged. Startup audio is checked as PCM;
it does not measure the T23's amplifier or speakers. Setup acceptance compares
the complete installed image, bounds all writes, then boots the HDD alone.
`scripts/qemu_test_cd_legacy_games.py` preserves the Wolf3D/DoomVan gameplay,
movement, cleanup, subsequent COM execution and per-game PCM checks.

A GRUB-decompress-to-MEMDISK handoff was evaluated and rejected: DOS commands
and Doom worked, but Windows exit terminated the shell. `raw`, `bigraw`, larger
MEMDISK stack and `safeint` variants did not resolve that regression. The same
payloads under GRUB's BIOS mover passed Windows and Doom interaction and return.
An intermediate local editing error in the EXEC snapshot word count was also
caught by the Doom return test and corrected before the final build. Neither
that candidate nor the MEMDISK candidates should be burned.

Some early new-test failures were harness errors (wrong COMMAND.COM location,
wrong COM demo marker, and QEMU's zero-sized WAV chunk headers). Their logs are
retained. The corrected harness reads actual command output and PCM payloads;
it does not accept a desktop or launch marker as proof that a command worked.

One final-ISO Doom run reached the title but failed to display the menu after
Escape. Six subsequent complete runs, including concurrent-load runs, passed.
The failing `final-doom` capture is retained; its cause is not established and
the successful repetitions do not prove that an intermittent fault is gone.
This remains a qualification on Doom acceptance and on physical T23 claims.

The GUI remains English, retains the original photo, and uses the tagline
“A modern Retro OS”. The VS Code scope retains MemoryMax=8,000,000,000 bytes
and MemoryHigh=7,000,000,000 bytes. Optical preflight and payload readback are
omitted as requested; the burn result is recorded separately.

## Written CD

`CiukiOS_full_cd_0-7-1.iso`: 149,358,592 bytes, SHA-256
`42b7edab34f01e501400d0053794eed6c7b6f74f23477b4f667aea6e61ecd168`.
The `/dev/sr0` write completed successfully with exit status 0 and ejection
requested. No separate optical preflight or payload readback was run.
`release-final.json` records 19 completed passing checks, the retained Doom
qualification and the burn result. Physical T23 validation remains outstanding.
