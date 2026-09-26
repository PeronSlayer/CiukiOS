# Native desktop and compressed GRUB4DOS boot

CiukiOS now opens its own graphical desktop at startup. The existing DOS
interpreter runs inside the same SHELL.COM process when selected; `EXIT` or
`DESKTOP` resumes the graphical desktop. COMMAND.COM retains its existing
nested `/C`, `/K` and process-exit behavior and is byte-identical to the prior
validated release.

## Controls and behavior

- F1: About/help; F2: Application Library; F3: Run; F4: full-screen DOS;
  F10: power dialog. Ctrl+Tab changes the library category.
- Arrow keys select applications, Enter opens the selection. Mouse selection
  and double-click are supported. The title bar moves the window; its corner
  resizes it. Minimize, restore, maximize and close perform real operations.
- Applications, Games and System expose the existing Windows, Doom, Doom
  Vanille, Wolfenstein, Costa, DOS Navigator, editor, display, sound and setup
  launchers. Files opens DOS Navigator. DOS/Windows applications run full-screen
  and return to the desktop when closed; this is not a multitasking window manager.
- Run supports insertion, Delete, Backspace, Home/End, cursor movement and
  horizontal scrolling. Shell output remains visible until acknowledged.
  Its input field redraws independently so keyboard input is not held up by
  repainting the entire desktop.
- The GUI follows DISPLAY.CFG at 640/800/1024 through validated banked VBE.
  The safe TEXT profile gives a native 640x480x16 VGA desktop and the existing
  text-mode DOS console. Windows uses the same existing display presets.
- Graphics and cursor state are private. INT33 state is saved/restored around
  every GUI session. Cursor background pixels are restored exactly; shared
  mouse handlers and application-specific mouse settings were not edited.
- All UI copy is English. Presentation remains “A modern Retro OS”. The boot
  photograph, progress bar and startup sound are retained.

## CD boot

The release is a BIOS El Torito CD using GRUB4DOS Legacy BIOS 0.4.6a
(2020-08-09, upstream tag `2020-08-09-0da21fe`). Its first menu entry expands a
gzip image into RAM and maps it as BIOS disk 80h. Existing physical disks are
shifted to 81h and above so SETUP still reaches the correct IDE device.
A recovery entry loads the original uncompressed image using MEMDISK. The
separate ISOLINUX recovery ISO is also retained.

The final raw image is 100,695,552 bytes (96.03 MiB), versus 46,622,080 bytes
(44.46 MiB) compressed: 53.70% fewer image bytes read from the CD. This is a
size measurement, not a physical T23 boot-time benchmark. QEMU validation is
at 512 MiB. The T23 still needs a hardware run of this new loader.

References: [upstream release](https://github.com/chenall/grub4dos/releases/tag/2020-08-09-0da21fe),
[GRUB4DOS manual](https://github.com/chenall/grub4dos/blob/0.4.6a/README_GRUB4DOS.txt).
The download and source archive are pinned by SHA-256. The CD carries COPYING,
the matching source archive, the source patch and the reproducible patch script.

### Raw INT13 / audio interrupt fix

The first unmodified GRUB4DOS test stopped Doom during R_Init. The captured
CPU state was in GRUB's resident INT13 code with CR0.PE enabled but the HDPMI
GDT still active. Upstream installs its temporary GDTR with interrupts enabled,
then executes CLI only just before setting CR0.PE. An HDPMI/audio interrupt can
replace GDTR in that interval. This explains the observed state.

The pinned binary changes one byte at offset 0x3946: `XOR EBX,EBX` becomes
`CLI; XOR BX,BX`, keeping every code offset unchanged. Only BX is consumed in
this path; the 64-bit branch clears EBX independently. Interrupts are masked
before saving/installing the GDT and restored after the caller's GDT is back.
The equivalent patch is `patches/grub4dos-int13-irq-gdt.patch` and applies to
the matching upstream source. The binary patch refuses every other revision.

Patched GRLDR SHA-256:
`818997093add6793c0145dea44a5ba8a337436a0fcb6652a7da1007ccca8e5d2`.
After the fix, Doom reaches the title/menu, responds after an idle interval,
and exits to the native desktop; Windows also starts and returns correctly.

GRUB4DOS also assigns an empty RAM-image MBR disk signature the value 0x80
(upstream `stage2/builtins.c`, RAM mapping path). Installer acceptance verifies
this exact four-byte identity field, the existing D: -> C: runtime patch, and
every other installed byte against the original source image.

## Build and evidence

Build with the already validated audio binaries:

```sh
CIUKIOS_SBEMU_MODE=reuse \
CIUKIOS_SBEMU_OUTPUT_DIR="$PWD/build/full/desktop-refresh-2026-09-06/tested-audio" \
bash scripts/build_full_cd.sh
```

`CIUKIOS_FULL_CD_BOOTLOADER=isolinux` selects the former release loader;
`CIUKIOS_FULL_CD_COMPRESS=0` can disable compression for diagnostics.
The optical disc is not inspected or read back by this workflow.

Release, payload hashes, screenshots and test logs are under
`build/full/ui-shell-2026-09-06/`. The release JSON records the final ISO hash.
Acceptance covers actual BIOS keyboard and relative PS/2 events, cursor pixel
integrity, window geometry, Run execution, DOS state/return, all three VBE
sizes and VGA fallback. Installer tests use disposable HDDs and compare actual
sector contents; the selected disk and untouched disks are checked separately.

The VS Code scope remains capped at 8,000,000,000 bytes, with MemoryHigh set
to 7,000,000,000 bytes.

Final acceptance logs: `desktop-release.log`, `doom-release.log`,
`recovery-release.log`, `files-release.log`, `setup-install-release.log`,
`setup-multiple.log`, `no-disk-release.log`, plus `windows-irq.log` and
`costa-ready.log` for the same native UI/loader changes. All report PASS.
The installer boots its new HDD into the desktop before testing DOS commands.
The final audio, game launchers, COMMAND.COM, SETUP.COM and VGASETUP.COM hashes
match the previously validated release byte-for-byte.

Final ISO: `build/full/CiukiOS_full_cd_0-7-1.iso` (149,331,968 bytes), SHA-256
`1dd53dfea9f1d3ca072c21bcc38167c42a141909b50eba816235f2808fb83a07`.
Written to `/dev/sr0` with `blank=fast -dao -eject`; xorriso exited 0 and
reported writing completed successfully. No separate optical pre-inspection
or post-write readback was performed. See `burn.log` and `release.json`.
