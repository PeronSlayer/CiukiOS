# Native desktop and Armada E500 work — 2026-09-26

This is a development change, not a declaration that the entire hardware and
windowed-DOS request is complete. The actual execution checks and their limits
are recorded below; QEMU cannot establish physical Armada E500 compatibility.

This is the **earlier desktop-platform record**. Its candidate images, sizes
and test results are retained as historical evidence. The selected later build
is documented in [native desktop repair](native-desktop-2026-09-26.md), image
SHA-256 `08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`,
with [eleven archived focused QEMU reports](validation/2026-09-26/validation.json).
It supersedes the empty public wallpaper catalog and DOS-only media path
described below: public builds now contain three CC0 patterns, Wallpaper accepts
99 converted tiles through Files/Refresh, and CD/floppy/BIOS disks have native
read-only browsing/import. USB still requires BIOS exposure before boot.
Kenney CC0 event sounds have native ICH AC97 and supported Sound Blaster playback.
The desktop also has a restricted BIOS-text DOS window and cooperative graphics
previews. Original DOS graphics/DPMI/audio virtualization and physical T23/E500
qualification remain open.

## Input and driver selection

The E500 has an i8042-compatible controller. The native initialization failure
path previously left the AUX clock/IRQ policy altered. The revised rollback
preserves the controller's actual translation/system settings, enables the
keyboard clock and IRQ1, and quiesces the failed mouse channel. Timeouts remain
bounded. A permanently unresponsive controller cannot be made functional by
inventing a command byte.

`\DRIVERS` contains the hardware detector, native audio/video helpers and the
rebuilt GPL CuteMouse driver, with its matching source and license. Existing
`\SYSTEM\DRIVERS` paths remain usable. On each boot the shell executes
`HWDETECT /Q /SAVE`, requires normal successful termination, validates the
version and completion marker of the fresh report, and selects the available
native startup-audio/video paths. It never accepts a previous machine's report
after detection fails. See [driver contract](driver-catalog.md) and
[hardware and licensing research](e500-drivers-2026-09-26.md).

Before inventory, `INPUTINI.COM` checks the installed mouse interface. A
successful native mouse stays in place. After a failed native initialization,
the kernel relinquishes its mouse vectors to the BIOS/inert detection stub;
the helper then tries `\DRIVERS\MOUSE\CTMOUSE.EXE /P /W /B` and verifies the
installed interface. This enables a generic BIOS PS/2 fallback without stacking
another driver over a functioning native mouse.

An actual QEMU boot with 128 MiB passed this fallback path: the packaged
INPUTINI and CuteMouse binaries installed the BIOS PS/2 driver, moved the
rendered cursor, accepted keyboard input and a mouse click, executed a DOS
command, and retained mouse operation after desktop return. The controlled
fixture changes only the kernel's existing `ENABLE_PS2_MOUSE_INIT=0` build
option; it demonstrates fallback selection, not a physical controller failure
or E500 qualification. The first harness positioning attempt overshot because
it assumed the native mouse's gain. Its failed artifacts were retained; the
passing run calibrated input from observed cursor displacement without
changing guest settings or RAM. Report:
`build/full/desktop-platform-2026-09-26/input-fallback-controlled-calibrated/report.json`.

ESS Maestro-2E in the E500 has no compatible backend in the shipped VSBHDA
version. It is reported as unsupported; an AC97 codec does not authorize using
Intel ICH controller registers. HP's Windows driver packages were downloaded
for inspection and are not represented as native CiukiOS drivers.

## Native applications

Files is a separate movable/minimizable window. It enumerates complete DOS
directories through paged results, opens supported files, creates directories,
renames, copies, moves and deletes with explicit confirmation. Copies advance
in bounded chunks between UI events, preserve an existing destination, and
remove a new partial destination on cancellation when the filesystem permits.
File launch returns through the existing shell dispatcher, without retaining a
return address on the shell stack. F4 returns to DOS; a pending copy is closed
and cancelled before handing control to a child.

The current kernel has one mounted DOS volume, without independent per-handle
drive bindings. Files therefore rejects other drive letters instead of showing
aliases as separate physical disks. Existing MEDIA tools remain available for
their explicitly supported read-only removable-media paths.

Tasks lists actual native windows and can switch to or close them. Its process
and memory pages inspect the DOS PSP/MCB allocation chain with bounds checks,
outside rendering. They do not invent CPU percentages or permit unsafe removal
of another program's memory. Native DOS EXEC is synchronous, so the desktop
cannot currently manage a running child. This is not a Windows-equivalent
preemptive process manager.

Open Tasks from the System page or with Ctrl+Shift+Esc. Files supports arrows,
Enter, F2 rename, F5 refresh, F6 move, F7 new folder and F8 delete; Tab/Enter also
operate its toolbar. The graphical interface remains in English.

All new interface code is project-authored. No Microsoft Task Manager or File
Manager executable has been copied into the implementation. The approved Ciuki
portrait, original splash photo and existing Tango icon set are retained.

## Wallpaper and rendering

Wallpaper is a native settings window with selection, Apply and persistent
configuration. The eleven owner-supplied BMPs are converted to lossless indexed
tiles at their original dimensions. High-color modes convert the palette to
the actual framebuffer masks; 8-bit modes map to the current desktop palette.
Painting touches only the current compositor band and performs no disk access.
Wallpaper storage is released before DOS and restored on desktop return.

Personal builds enable these Windows-origin files explicitly with
`CIUKIOS_PERSONAL_WALLPAPERS=1`. No redistribution license was supplied for them;
the default build packages an empty wallpaper catalog. See
[wallpaper formats and provenance](../assets/wallpapers/README.md).

Window geometry, dragging damage, clamping and the taskbar now handle eleven
native windows, including the larger Files frame. The taskbar fits its buttons
to the available width. The desktop font and approved portrait pixels are
packed byte-for-byte in `\SYSTEM\UI\DESKTOP.DAT`, outside the near code segment.
They are released before DOS/EXEC and reloaded on return. COMMAND.COM remains
independent of graphical resources. The shell and kernel size guards remain
enabled; neither memory boundary was expanded.

## Verification and remaining work

- Initial QEMU execution, Pentium III/128 MiB/800×600: Files opened, a folder
  was created through the UI and listed by DOS, COMDEMO executed, and the
  desktop returned. Actual screenshots and the fresh hardware report are in
  `build/full/desktop-platform-2026-09-26/first-run/`.
- Focused instruction-level input checks exercise production assembly against
  a modeled 8042: 48 rollback cases, including timeout and malformed ACK paths.
  The old kernel fails the new rollback assertion. These are not physical input
  tests. See `scripts/test_ps2_rollback.py`.
- Ten CWD/GETCWD boundary checks exercise the production path parser with
  modeled directory metadata. They caught a lost SI in the fast path, now fixed,
  and verify 63-byte paths and rejection of overflow. They do not substitute
  for filesystem integration checks. See `scripts/test_cwd_limits.py`.
- The final packaged Files/Tasks lane passes on the candidate image, without
  replacing guest binaries: actual GUI create/copy/rename/move, invalid-path
  and cross-drive refusal, preservation of existing files, cancellation and
  confirmed deletion. Independent FAT reads check metadata and every copied
  byte, released empty-directory clusters and agreement of both FATs. Three
  COMDEMO launches return with identical SS:ESP; Task Manager tabs, window
  controls and Ctrl+Shift+Esc work. Report:
  `build/full/desktop-platform-2026-09-26/final-native-utilities/report.json`.

### Filesystem defects found by the execution test

The first real Files test exposed pre-existing kernel defects: MKDIR published
an all-zero directory without `.`/`..`, and RMDIR removed nonempty directories
without checking their contents or reclaiming empty-directory clusters. The
failed test's parent link disappeared while the moved file's bytes survived
in an orphaned allocated cluster. The isolated negative disk and its independent
byte reconstruction are preserved under `native-utilities-4/`.

The corrected kernel initializes canonical directory entries, checks the whole
directory and its bounded FAT chain before mutation, refuses dot aliases and
the current directory, and reclaims a deleted empty directory's chain. DEL
cannot bypass the directory check. Cross-parent directory moves through AH56
are rejected before writing; same-parent renames and regular file moves retain
their existing semantics. Seventeen modeled sector/FAT cases and a separate
three-case rename guard check complement the actual GUI/FAT execution tests.

The IRQ1 BIOS chain now preserves full 32-bit general registers and DS/ES/FS/GS
on both normal and nested paths. Six production-instruction cases use a modeled
legacy BIOS that clobbers upper halves and FS/GS; the old kernel fails and the
guarded wrapper passes. This demonstrates context preservation, not that a
particular laptop BIOS caused the reported crash.

The final kernel is 43,169 bytes (95 bytes below its unchanged boundary); the
shell is 60,208 bytes, including its private stack and below its existing COM
limit. The final personal HDD development image is
`build/full/desktop-platform-2026-09-26/ciukios-candidate.img`, SHA-256
`f2aa95a62bc4830d4c7560aec94524b868730b8a1c4c40ae618a547ea8eb001d`.
Packaged file hashes and exact build objects are retained alongside it.

The image recorded above predates DOS windows. Subsequent work adds a
**single foreground BIOS-text DOS window**; its separate image, ownership
contract and qualification are recorded in
[the runtime document](dos-window-runtime-2026-09-26.md).
Direct-video text applications, graphics and DPMI remain outside that tier.
The existing fullscreen execution path is retained. General windowed DOS
compatibility needs a V86 monitor, video and input virtualization, DOS
serialization and a renderer that can operate under the monitor. Importing
Jemm/HDPMI alone does not provide those services.
The inspected upstream code, compatibility gates and three bounded parallel
work prompts are in [the DOS-window architecture document](dos-window-architecture-2026-09-26.md).

Physical E500 input and ESS audio, arbitrary DOS graphics/DPMI applications in
windows, and independently mounted native file-manager volumes remain explicit
limitations. No physical disk was rewritten and no CD was burned by this work.

The new project-authored/GPL components and the wallpaper packaging policy do
not establish redistribution rights for an entire personal image. Existing
owner-provided Windows media and commercial games require a separate release
audit before a public OS distribution.


## Wallpaper verification

Actual packaged QEMU runs passed at 800x600x8 and 1280x1024x32 with Pentium III
and 128 MiB configured. Real input applied the 256x256 and 160x160 tiles, moved
and closed a Run window, executed COMDEMO in DOS, returned to the desktop and
cold-booted the same HDD copy. Six screenshot checks per depth found **zero
differing pixels with zero tolerance**: 195,528–304,800 pixels per 8-bit check
and 898,688–1,007,960 per 32-bit check. All 11 packaged tiles also decode to their
original BMP pixels exactly. No binary, asset, configuration or RAM injection
was used; VGASETUP selected each mode through its actual confirmation workflow.

Reports: `build/full/desktop-platform-2026-09-26/wallpaper-8-exact/result.json`,
`wallpaper-32/result.json` and `wallpaper-cpu/result.json` in the same build
folder. The CPU report separately covers production palette/band instructions
for 15/16/24/32-bit formats, wrap/bounds and register preservation; it is not a
BIOS or GPU test. These results do not qualify physical T23 hardware or unrelated
filesystem operations. All wallpaper VMs were closed.

Tested source image SHA-256: `ad1cc11ba7e1f3506f0e321f6eda5415d515f96a393781080a07dff5c50421fa`.
Packaged SHELL.COM SHA-256: `77a091c4ccad79bfcace4b8fe3c443756d283cd03018b4b1aa2614371f1c2223`.
Detailed reproduction, DAC oracle and screenshots: [wallpaper validation](wallpaper-validation-2026-09-26.md).

The final candidate was then checked again at 1280x1024x32 after the kernel
repairs and font/logo resource-release change. All six actual input/screenshot
stages passed again with zero differing pixels, including DOS return and cold
restart. This run used candidate `f2aa95a6…001d` and packaged shell
`9454f6d6…8bd`, without any binary or RAM overrides. Its full hashes, commands
and results are in `final-wallpaper-32/result.json` in the same build folder.
The 8-bit run above remains evidence for the recorded earlier image; it was
not repeated on the final candidate. Neither run establishes native 2K or
physical laptop compatibility.
