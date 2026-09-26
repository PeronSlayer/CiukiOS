# UI, display setup and hardware restart — 2026-09-06

**CD burned on 2026-09-06 after the user explicitly lifted the burn hold
("masterizza intanto e poi dimmi come fare la copia dell'HDD"). The physical T23
subdirectory/EXEC failure is not yet explained or certified fixed.** The user confirmed that it persists
from a complete power-off with no CD, while `DIR \` shows the root directory
correctly. Subdirectories show corrupt names; the previously reported root
COMMAND.COM execution also fails. A successful emulated installation does not
settle those physical observations.

The frozen `CiukiOS_candidate.iso` (SHA256
`ec79babd5dc6158869ed73c9ad6108a9194d7cf28de62680e95f462faa3eaee8`)
was written to the inserted CD-RW on `/dev/sr0` using xorriso, with fast blanking
as needed, DAO and eject requested. Xorriso reported successful writing and a
closed session, and exited with code 0. No optical readback verification was
performed, as requested. The command, timestamps and result are recorded in
`manifest.json`; the complete writer log is
`build/full/ui-setup-repair-2026-09-06/burn-20260906T190822Z.log`.
The T23 HDD has not been acquired: preserve its failing installation before
reinstalling so that the directory/FAT/data sectors remain available to diagnose.

Evidence is in `build/full/ui-setup-repair-2026-09-06/`. The preceding keyboard
LED/EXEC error-handling fixes are retained; this change does not alter that
kernel, the games, Windows files, or audio payloads. `payload-changes.json`
compares all 1408 files against the preceding installed candidate. Only
SYSTEM/SHELL.COM, SYSTEM/DRIVERS/VGASETUP.COM and APPS/SETUP.COM differ.

## Product changes

- AUTO enumerates the firmware's usable VBE modes even when DDC/EDID is absent,
  instead of silently imposing an 800x600 ceiling. Valid EDID preferred timing
  still bounds the choice. Geometry, pitch, pixel layout and bank checks remain.
- The desktop selects a mode at least 800x600 when available, even if DOS has an
  old TEXT/0640 profile. The explicit recovery mode remains available. A saved
  supported 800/1024 graphics choice is still respected.
- VGASETUP maps the current mode's dimensions to a **bounded menu index**. Its
  previous `(mode_id - 101h) / 2` calculation put OEM/direct-colour modes outside
  the four-entry option table. Enter then printed program bytes as the preview
  label. This is reproduced with the unchanged old executable in
  `vgasetup-before-reproduced/`: 38868 invalid text bytes. The fixed executable
  previews 1024, cancels on its real timer, preserves both configuration files,
  applies a confirmed 800 profile, and returns to usable DOS (`vgasetup-final/`).
  Its private stack now has 4 KiB rather than 1 KiB for firmware/interrupt calls;
  this is hardening, not a measured T23 stack requirement.
- Setup uses banked 800x600 with the same palette and original proportional fonts
  as the desktop. Text is drawn at its original pixel size without stretching.
  The COM program releases unused arena space before allocating its 60 KiB
  drawing buffer. Scenes are composed into bounded bands, then only changed,
  completed bytes are copied to VRAM. Disk progress damages only its own panel.
  The graphical-only VBE build omits unused console routines and fits the
  existing 48 KiB setup slot (46576 bytes).
- Restart reaches the reset sequence directly, without first calling a video
  BIOS mode restore that could block. It requests chipset CF9 reset, allows I/O
  settling time, and retains controller reset fallbacks (8042, port 92, CPU
  triple fault). There is no software jump to the BIOS entry point as a final
  substitute for reset. The existing CF9 approach follows Intel's
  [ICH3 reset register documentation](https://www.intel.com.br/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf#page=329).

VBE mode enumeration follows the [VESA VBE 2.0 standard](https://www.phatcode.net/res/221/files/vbe20.pdf#page=14):
firmware mode IDs must not be treated as application menu indices. The output
remains banked real-mode graphics; no flat segment or new resident UI hook is
introduced.

## Verification and limits

- `native-auto/`: actual Cirrus AUTO desktop is 1280x1024. Eight opening frames
  retain unrelated desktop pixels; overlapping windows, retained text, live
  dragging, minimize/restore, frame controls and four cursor limits pass.
- `native-text-final/`: the final shell also selects 1280x1024 when the saved DOS
  profile is TEXT; the same window/input/edge tests pass, and DOS still works.
- `rendering-final/`: sixteen frames per focus/radio/page transition; fixed
  chrome and unrelated pixels remain identical. Navigation, confirmation
  invalidation and cancellation pass; the disposable target is unchanged.
  Cursor hiding is measured separately from screen damage, not silently counted
  as stable pixels. These are sampled frames, not a measurement of the T23 LCD.
- `quick-format/`: real metadata-only format; FAT copies agree, old file-data
  area remains A5, and bytes beyond the displayed partition are untouched.
- `full-format/`: real full clearing plus readback; the file-data area is zero,
  FAT copies agree, and bytes beyond the partition remain A5.
- `install-final/`: final ISO, full format, real installer copy and readback.
  Every installed sector matches the source, with only the documented D: to C:
  boot-byte patch and GRUB RAM-disk identity. The test clicks Restart with a
  real PS/2 pointer, observes QMP `RESET` with `guest=true, reason=guest-reset`,
  and the same VM starts the installed HDD without a CD and executes DOS.
  It never uses a monitor `system_reset` command to manufacture that result.
- `io-error-final/`: an actual block read error stops Setup and leaves the
  incomplete target without a valid MBR.
- `paths-final-shell/`: independent FAT parsing, all eight subdirectory listings,
  complete SBEMU file checksums, relative paths, COM/MZ execution, concurrent
  PS/2 activity, startup PCM and a comparison of 38675 immutable kernel bytes.
  This uses the final shell on the first installer-produced HDD; the final-image
  repetition is separately recorded in `hdd-final-paths/`: all 340 entries in
  eight subdirectories match, with 1103 concurrent pointer commands and no
  unexpected kernel-code changes. This final run replaces no product payload.
- `windows-final/`: actual Windows 3.1 on the final installed HDD, two sessions,
  Calculator operations, resize/repaint, WAV and MIDI, and DOS return all pass.
- `planar-control/`: the preceding native shell in VGA planar mode 12h (TEXT
  profile), followed by full path/file/COM/MZ checks and concurrent mouse motion,
  also passes. This explicitly covers the VGA fallback without assuming that a
  640x480 VBE test exercises the same renderer.

## Retained failed trials / controls

No failed trial has been relabeled as passing:

- `preview/`: the first VBE installer could not allocate its buffer because the
  COM program had not shrunk its initial arena. The product was corrected.
- `preview2/`: first successful rendering check before the final palette and
  progress-only damage path. The old default palette was subsequently corrected.
- `build.log`: 49984-byte setup exceeded its 49152-byte slot. Omitting unused
  console code corrected the build; the slot/layout was not enlarged.
- `vgasetup-before/`: the initial negative assertion expected mode rejection.
  The actual defect was a successful preview with an out-of-table, corrupt label.
  The corrected negative observation is `vgasetup-before-reproduced/`.
- `vgasetup-after/`: the validator compared the Windows directory to its state
  before SYSTEM.INI was legitimately renamed to SYSTEM.BAK. The corrected test
  checks the permitted filename change and the actual independently parsed
  on-disk enumeration, then verifies guest COMMAND.COM and program execution.
- `vbe-before/`, `vbe-after/`: an investigation of the legacy kernel INT 10h hook
  did not explain the failure. Native SHELL and its children use the restored
  BIOS vector, so both exercised the real BIOS successfully. The experimental
  kernel edits were **reverted** and byte identity with the preceding kernel
  checked. `discarded-int10-control.sys/.lst` are experiments, never release
  payloads. `candidate.sys` is the actual unchanged final kernel.

The current evidence establishes the UI/setup/display fixes above. It does not
establish why only the T23's subdirectory/data reads fail. No sector repair or
filesystem-format change has been guessed from a passing QEMU test.
