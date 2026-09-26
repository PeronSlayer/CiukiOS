# Installed HDD regression qualification

The reported machine boots CiukiOS from its installed hard disk. The new
`scripts/qemu_test_installed_hdd.py` therefore boots a disposable copy of the
actual installed whole-disk image as the only hard disk, with no CD and no
resident GRUB disk mapper. Each QEMU guest has 512 MiB. The original installed
image is checked for unchanged SHA-256 after every run.

Investigation evidence is under `build/full/hdd-regression-2026-09-06/`. Each case preserves
the guest disk, serial output, screenshots and captured AC97 PCM. The result
JSON records the actual guest kernel, shell, launcher and audio-driver hashes;
`completed` becomes true only after the final disk and PCM assertions pass.
Failures retain CPU/PIC registers and conventional RAM as well.

## Final installed HDD

All eight cases passed on independent copies of
`build/full/hdd-release-final-2026-09-06/install-quick/target.img`, installed
through graphical setup from the final ISO. No kernel, shell or launcher was
replaced for this matrix. The only injected executable payloads are the
file-checking fixture and, in its dedicated case, the BIOS fault TSR. Storage
and video-menu cases select the TEXT profile as described below.

The source HDD SHA-256 is
`181a59299cdb122e8f2f4436ae86de2bf596e4250ea63e7d626d13507e08ddca`.
The installed shell SHA-256 is
`e5d54d2e25f67fa917cba0d22f66044eb0faf2cd394f84ca94d9ed5e32aaa700`;
the kernel SHA-256 is
`7c482ad57a0bb44d3456fb0a5e999d13bbe5743468731a4e739f5186f671d1e3`.
The controller checked these hashes and the Doom launcher hash before the
matrix and again in every guest result.

Final logs, guest disks, screenshots, PCM and per-case results are under
`build/full/hdd-release-final-2026-09-06/installed-acceptance/`.
Its `summary.json` records all eight completed cases: filesystem, paths,
BIOS fallback, video menu, automatic video, games, legacy applications and
original Doom. The filesystem checks cover 437 directory entries and
25,649,394 bytes across twelve complete files. Automatic 1280×800 DIR returns
in 0.974 seconds including typing. Wolf3D, Doom Vanille and original Doom
pass interaction, gameplay and return, with separate PCM RMS measurements
of 560.8, 1142.1 and 1057.3 respectively. All eight cases leave the complete
38,661-byte main kernel code prefix unchanged, including the XMS entry stub.

The same shell has separate deterministic RTC-minute and complete native
window checks, documented in `native-windows-repair-2026-09-06.md`.
The overall release index, including Windows and installation checks, is
`hdd-release-2026-09-06.md`.

## Earlier candidates

The first release candidate was installed through graphical setup, then all eight cases
passed on copies of that freshly installed HDD with no kernel, shell or
launcher replacement. Its evidence and the aggregate `summary.json` are in
`build/full/hdd-release-2026-09-06/installed-acceptance/`. The source HDD SHA-256
is `f2fecb3535941b1e30ef206ec553fcf6926105b8e5e5d2717e4ba95f9c0670ce`.
That filesystem run checked 437 directory entries and 25,649,394 bytes
across twelve complete files. Automatic 1280×800 DIR returned in 0.970 seconds.
The three games produced separate PCM RMS measurements of 562.1 (Wolf3D),
1141.0 (Doom Vanille) and 1045.2 (original Doom), with successful interaction
and return. All eight cases retained the 38,661-byte main kernel code prefix.
That candidate was subsequently rejected by a separate test which kept Run
open across an actual RTC minute change: the clock repaint retained ES=0040
and the Run field copied 18 bytes into the kernel. The final shell corrects
both the clock's segment lifetime and the field's local destination segment.
The preserved negative control is
`build/full/ui-multiwindow-2026-09-06/clock-before/results.json`; the repaired
800×600 and AUTO32 controls are `clock-after-final/` and
`clock-after-auto-final/` under the same evidence directory. A passed earlier
matrix does not supersede that later, more specific failure.

## Reproduced faults

1. **BIOS disk fallback could return the wrong directory or corrupt a write.**
   The test-only `disk_bios_fault.asm` TSR leaves the real BIOS geometry and
   CHS data transfers in place, but makes EDD reads/writes fail after changing
   the DAP transfer count to zero. Successful AH=08 geometry calls return
   ES:DI=F000:1234, deliberately modeling a firmware register-output variation
   different from the caller's disk buffer. The documented parameter-table
   output applies to floppy drives; this test does not claim every HDD BIOS
   returns it. With the old kernel, `DIR \WINDOWS` displayed entries
   from APPS instead. Evidence: `bios-before/`. With the repaired disk code,
   actual DOS reads, directory traversal, COPY and execution work under these
   same BIOS conditions. The copied 77-byte MZ and 245,760-byte PCM are also
   compared byte-for-byte using an independent host FAT16 parser.

2. **A newly loaded MZ could make the memory allocator overwrite kernel code.**
   Adding one resident paragraph to the test TSR changed the following MZ
   address and made execution reproducibly fail on return. The MZ's bytes and
   relocations were correct. A hardware watchpoint caught
   `int21_mem_write_chain_entry` writing an MCB at physical 0x3C30, inside
   INT21 code. The published new PSP still contained old program bytes;
   following its uninitialized parent field walked 1CA5→C283→03C4→2E02.
   The allocator treated 03C4 as the lowest PSP. Evidence:
   `bios-final/`, `bios-newfixture-oldshell/`, and `mz-mcb-watch/trace.log`.
   This occurred with both the old and new native shell. Initializing the
   minimal PSP before publishing the identity, together with validating PSP
   ancestry, fixes this reproduced cause. The new BIOS fixture now passes at
   four successive resident paragraph offsets (`bios-mcb-pad0/` through
   `bios-mcb-pad3/`). All 38,661 bytes of the assembled kernel's main code
   prefix remain byte-identical in RAM after each run.

3. **High-color DOS output was unusably slow.** The old automatic 1280×800
   console exceeded 90 seconds partway through `DIR WINDOWS`. The repaired
   shell completes all 116 entries and returns to a working prompt in 0.974
   seconds including command typing. The following ECHO and final screen are
   checked, so seeing an early prompt alone does not count. Evidence:
   `baseline/` and `video-after-2/`. Rendering details and additional native
   window tests are documented in `native-windows-repair-2026-09-06.md`.

## What the tests establish

- `filesystem` compares every displayed short-name entry in nine application,
  Windows and system directories with an independent FAT16 traversal. It then
  opens and reads twelve entire guest files, including both complete Doom
  WADs, and compares byte counts and FNV-1a checksums against the image. It
  executes both COM and MZ programs and changes video mode from DOS.
- `paths` changes to lowercase Windows and game paths, runs relative DIR and
  reads a file relative to each selected directory, checks CD.., then executes
  COM and MZ again. This covers the current-directory sequence in the report.
- `video-menu` opens VGASETUP by name from APPS, previews 800×600, cancels,
  reads back the unchanged display profile and Windows SYSTEM.INI, reopens
  setup and confirms 800×600. The final guest files are checked independently.
- `games` enters actual Wolf3D and Doom Vanille gameplay, checks scene changes
  from held movement, sends shooting input, quits, verifies launcher cleanup
  and executes a subsequent COM. Per-game PCM windows must contain audio.
- `legacy-apps` checks the actual DOS Navigator directory rows and panel
  switch, exits, then starts Costa, checks visible cursor movement and exits.
  A subsequent COM must work after each application.
- `classic-doom` checks the original engine's title, six-row menu, selected
  menu cursor, episode/skill selection, gameplay movement, shooting, quit,
  launcher cleanup, subsequent COM, desktop mouse movement and DOS return.
- Startup audio is checked in the PCM captured before the desktop-ready
  boundary, separately from game audio. This establishes guest digital
  playback; it does not measure the physical T23 codec amplifier or speakers.
- With a supplied kernel and its NASM listing, the validator compares the
  assembled main-code prefix with final guest RAM. Only HDPMI's documented
  five-byte XMS entry hook may differ. An MCB write anywhere else fails.

The boot-only installed-HDD checks used previously were insufficient to
establish these properties. The source disk, filenames, screenshots and
successful launch messages alone are not used as substitutes for exercising
the actual application and returning to a working shell.

## Original Doom audio interrupt qualification

The original Doom launcher now passes VSBHDA `/CF4`, which masks the PIT IRQ
inside the sound hardware interrupt and restores its previous mask before
returning. This documented option is local to original Doom; the engine,
VSBHDA and HDPMI binaries and the other launchers are unchanged. References:
[VSBHDA options](https://github.com/Baron-von-Riedesel/VSBHDA/blob/main/vsbhda.txt)
and the pinned local `SNDISR.C` / `STACKISR.ASM` implementation.

The previous launcher intermittently reached the title and then ignored
Escape, with master PIC IRQ0 left in service (`classic-doom-after/`). Other
unchanged-launcher runs passed, so an occasional pass does not establish a
fix. Six cold starts with `/CF4` passed the complete menu, gameplay, sound and
return sequence (`classic-doom-cf4-1/` through `classic-doom-cf4-6/`), including
four with desktop/mouse/DOS return. These results establish compatibility of
the mitigation; they do not prove exclusive causality or eliminate every
intermittent hardware failure.

These are reproducible emulator checks of the installed HDD and specified
firmware variations. Windows WAV/MIDI/repaint qualification has its own
`qemu_test_hdd_windows.py` evidence. Physical T23, USB-controller and speaker
behavior require their own measurements; no universal application or hardware
compatibility claim follows from this test matrix.
