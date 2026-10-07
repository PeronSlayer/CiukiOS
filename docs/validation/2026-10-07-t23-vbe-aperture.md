# ThinkPad T23: framebuffer aperture and V86 rendering

The owner reports that the latest full HDD image still shows the desktop in
a narrow corrupted strip on the ThinkPad T23 (S3 SuperSavage IXC, PCI
5333:8c2e). The photo establishes the physical failure, but does not establish
the active BIOS mode or framebuffer path. QEMU cannot qualify this S3 BIOS.

## Research and implementation decision

- [VESA VBE 3.0 specification](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf),
  Function 03h, page 44: BX D14 reports windowed versus linear access. It is
  not reserved. Restore checking the returned mode and access model before
  rendering. Function 05h, pages 46–47, positions the window in granularity
  units; `PhysBasePtr` describes flat framebuffer access, not permission to
  bypass a banked mode's hardware window.
- [QEMU standard VGA specification](https://www.qemu.org/docs/master/specs/standard-vga.html)
  documents PCI 1234:1111's framebuffer at BAR0. [SeaBIOS Bochs VGA source](https://github.com/qemu/seabios/blob/master/vgasrc/bochsvga.c)
  selects BAR2 for virtio-vga (1af4:1050). Restore this device/BAR allowlist
  for banked-to-linear aliases. Retain the expanded PCI bus search and the
  1 MiB physical-address minimum: neither establishes an alias by itself.
  Apply the same check to both the desktop's A000 PTE remapping and the
  CVSESSION framebuffer binding. A hypervisor bit is insufficient evidence.
- [Linux Savage DRM source](https://github.com/torvalds/linux/blob/v5.10/drivers/gpu/drm/savage/savage_bci.c)
  distinguishes SuperSavage BAR0 MMIO, BAR1 framebuffer, and BAR2 aperture.
  Matching any GPU BAR is therefore not proof that writes reach scanout.
  The handed-off `cvlegacy_vbe_alias` accepted any such match, contrary to
  its documented contract. Restore the documented allowlist. On physical
  S3 in a banked mode, retain INT 10h/4F05
  and the ordinary A000 window instead of claiming the QEMU alias.
- [Intel Software Developer's Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  MOV to/from control registers: local CR0 transitions cannot execute in
  virtual-8086 mode. Restore the real-mode-only LFB eligibility check;
  finding CVSESSION alone does not make the burst entry or fill privileged.
  MOVS with an effective address beyond a segment limit faults; the handoff's
  assertion that a lost 64 KiB limit silently wraps every 32 scanlines is
  not supported by the instruction semantics.
- VBE 3.0 page-count fields are zero-based: zero is one usable page, not an
  absent value. Preserve zero when a supplied linear pitch establishes the
  extended layout, and always honour the banked page count. Keep the prior
  fallback to standard pitch/masks for a zero linear pitch/mask descriptor.

These changes address reproducible contract violations. The causal link to
the photographed artifact remains a hardware hypothesis until a new image
is booted on the T23. Retain the single-page fallback and add regressions for
S3 alias rejection, V86 eligibility,
mode readback, and zero page counts. Record runtime and release checks below
after rebuilding the canonical full HDD and sanitized Windows bundle.

The burst audit contradicts the handoff's explanation for removing `MOV ES,0`.
[Intel SDM Volume 3A, section 9.9.2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf)
requires loading ordinary 64 KiB descriptors **before** clearing PE when
returning to ordinary real mode. A real-mode ES load changes its base;
it does not substitute the protected-mode descriptor's limit.
[QEMU's real-mode segment-load implementation](https://github.com/qemu/qemu/blob/master/target/i386/tcg/translate.c)
(`gen_op_movl_seg_real`) likewise updates selector and base only. The
renderer intentionally uses ES for scratch cell rows inside a burst, and
transfer epilogues restore it. Removing the base-zero reload can direct
linear writes to `PhysBasePtr + (scratch_segment << 4)` instead of scanout.
Restore the base resets at flat entry, copy and fill, keep ordinary ES
preservation in transfer/fill epilogues, and bypass the banked-only CVSESSION
service for real LFB transfers. The outer protected-mode exit still explicitly
loads the 64 KiB descriptor before restoring the caller's ES. A full-HDD KVM
CPU fixture checks multiple production copy/read/fill operations in one
burst, real-mode scratch ES loads, exact physical addresses, canaries across
the 64 KiB boundary, and the caller's ES after the burst.

The shared CiukiDOS INT 10h handler also reverses the documented 4F05 BH
operation / BL window parameters in its local bookkeeping and fallback.
More significantly, after a real firmware mode set it replaces a failed
firmware bank switch with an in-RAM emulation and copies that bank into the
same physical A000 window. This can overwrite the first 64 KiB repeatedly
while reporting success. Track firmware ownership of the active mode and
return failure when its bank switch fails; only a locally established mode
can use local backing-store emulation. Correct BH/BL decoding and preserve
DX across the bookkeeping after 4F02, as VBE requires. This is shared kernel
work for the full HDD profile; no standalone floppy build is involved.
The local 4F02 path also restores its checked mode ID/table pointer after
writing the BDA and preserves DX while initializing bank zero.

The NOVM renderer test exposed another production bug before the first
frame: `vc_session_find` queried INT 2Fh/1684h with the caller's nonzero
ES:DI. An absent VMM may leave those registers unchanged, which the renderer
then cached and called as a CVSESSION entry. The
[original RBIL Function 1684h contract](https://fd.lod.bz/rbil/interrup/windows/2f1684.html)
requires ES:DI = 0000:0000 on input. Clear both before discovery and preserve
the caller's registers as before. Diagnostic copies on the full HDD traced
the loader return specifically to `vc_session_fb_bind`; its false entry is
independent of the LFB copy routines. The NOVM graphical gate will verify
that absent-service discovery leaves the LFB desktop usable.

## Validation

All production changes were built into the canonical full HDD with:

```sh
systemd-run --user --scope -p MemoryMax=3G -p MemorySwapMax=1G -- make build-full
```

Host memory was checked first (4.1 GiB available before the final build).
Full builds, QEMU runs and ZIP verification were sequential and capped at
3 GiB memory / 1 GiB swap. No standalone floppy profile or CD build was run.
No guest RAM reservation or DOS/VM memory limit was changed by this fix.

| Check | Result and scope |
| --- | --- |
| `scripts/test_session_gpu_legacy_host.sh` | 10 production-C checks pass: known QEMU/virtio aliases accepted, SuperSavage and unrelated/wrong BARs rejected. |
| `scripts/test_vbe3_metadata.py` | 13 production-instruction checks pass: layout/page-count fallback, V86 LFB refusal, mode/access-model readback, absent/present INT 2Fh discovery. BIOS descriptors are supplied by the fixture. |
| `scripts/test_vbe_auto_bounds.py` | 11 checks pass, including unknown/invalid EDID using the retained 1024×768 AUTO bound. |
| `scripts/test_int10_vbe_window.py` | Extracted production handler passes firmware and local 4F02/DX, BH/BL/window B, failed firmware bank-switch rejection, and fake local-LFB rejection. Firmware/backing-store calls are stubbed. |
| `scripts/qemu_test_vbe_aperture.py` | Three full-HDD boots pass at 128 MiB / Pentium III: standard VGA with CVSESSION, Cirrus with real BIOS banks and no alias binding, standard VGA NOVM with real-mode LFB. The assembled shell must match the shipped shell byte for byte. |
| `scripts/qemu_test_vbe_es_base.py` | Full-HDD DOS! boot passes production LFB copy/read/fill operations in a single burst, scratch ES reloads, physical canaries across offset FFFFh, and restoration of caller ES=3456h. Uses an owned 128 KiB DOS block. |
| `scripts/qemu_test_vbe_contract.py` | Full-HDD BIOS calls preserve DX, set/query bank 2, retain VRAM in the correct bank and return to a usable DOS console. |

In each graphical case, real PS/2 input moved the Run window to Y=505.
Rendered content begins at Y=545; 73,253 pixels below scanline 512 changed
correctly. The measured top/left bevels contain 465/225 expected pixels and
the taskbar rule remains visible across all 1,024 bottom-screen pixels.
No guest RAM was patched to select a rendering path. Each runner verifies
that its source HDD image remains unchanged.

Final evidence is under `build/full/t23-vbe-fix/`:

- `build-final.log`: successful full build and refreshed portable ZIP.
- `metadata/results.json` and `auto-bounds/result.json`: CPU fixture results.
- `render-canonical/results.json` and per-case desktop/moved-window PNGs.
- `es-canonical/report.json` and `bank-canonical/result.json`: full-HDD CPU/BIOS checks.
- `release-verification.json`: ZIP CRC, manifest/executable/image hashes,
  matching kernel/shell, absence of all eight excluded private payload paths,
  and all 27,773 free FAT16 clusters zeroed.

The portable ZIP contains 3,331 entries. Its manifest accurately records an
uncommitted source worktree and `windows_runtime_tested=false`. Archive
verification does not constitute testing its launcher on Windows.

Final SHA-256:

```text
45891bd1ca9bdf225113316d8bc6b6dcfa01e1448c7e74d6d03908590d5c4f7f  build/full/ciukios-full.img
03455095f0caa097a09371cc16356643fb708bb1eade2ee6cf0fd37148969f40  build/releases/CiukiOS-0.8.3-Windows-portable.zip
```

The photographed T23 failure is **not yet physically requalified**. The
checks establish corrected software contracts and working emulator paths;
boot this new full HDD image on that T23 to determine whether its S3 BIOS
now renders correctly.

## Physical disk preparation

At the owner's subsequent request, the validated canonical HDD image was
written from offset zero to the raw `/dev/sdc` device: Transcend
TS64GMSA230S, serial H551120730, 64,023,257,088 bytes. The stable device ID,
disk sequence 54 and unmounted state were checked before writing. Linux
authentication granted the exclusive writable UDisks handle.

All 134,217,728 image bytes were synchronized, then reread with direct I/O
and compared byte for byte against the source. Readback SHA-256 equals the
canonical hash above. The USB disk was powered off for removal. Evidence:
`build/full/t23-vbe-fix/physical-write.json`. This prepares the physical
support; it does not establish a successful T23 boot or qualify its S3 BIOS.

## Returned T23 disk: unresolved resolution and Doom failures

The owner returned the same TS64GMSA230S / H551120730 disk after the next
T23 boot and reported that changing resolution still fails and Doom raises
a JemmEx exception. On 2026-10-07 it was mounted through UDisks read-only
at `/run/media/peronslayer/CIUKIOSFULL` (disk sequence 55). The acquisition
did not modify or reflash the disk. Evidence was copied into
`build/full/t23-vbe-fix/physical-logs-20261007-111516/`; `manifest.json`
records mount options, file sizes, timestamps and SHA-256 hashes.

The physical `DRIVERS/LOADDRV.LOG` contains:

```text
NE2000 SKIP no matching hardware
PCNET SKIP no matching hardware
RTL8139 SKIP no matching hardware
S3VBEFIX OK PCI 5333:8C2E IRQ 0x0B
```

This confirms driver-loader success on SuperSavage, not successful VBE
mode switching. `SYSTEM/BOOT.LOG` is zero bytes; no runtime Doom dump or
display-change log was found. The existing `app_log` API documents COM1
output, so those application markers are not evidence saved on this disk.
`SYSTEM/VIDEO/DISPLAY.CFG` remains `1024`; `SYSTEM/STARTUP.CFG` is `LIVE`.

The physical `SYSTEM/MEMMAP.BIN` is a complete CMAP v1 report with 10 E820
entries: 636 KiB conventional memory, EBDA segment 9F00h, and 535,166,976
usable bytes above 1 MiB (510.375 MiB). The ranges have no overlap. No
guest memory limit is changed on the basis of this evidence. All twelve
compared production executables, including shell, Display, DOSVM, VMFORK,
DPMIRUN, CVSESSION, JemmEx and both Doom wrappers, match the canonical
image byte for byte (`binary-comparison.json`).

The photograph records exception 0Dh with CS:EIP=8237:00010000,
SS:ESP=8237:0000EFF8, EFLAGS=00033286 and CR0=80000011. Upstream
[Jemm's saved-client-frame exception handler](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/JEMM32.ASM)
prints this V86 client state. An offset of 10000h exceeds the V86 code
segment's 64 KiB limit; the photograph alone does not establish which
instruction or saved-frame operation caused it. It also contains no VBE
mode-set registers to establish a relationship with the resolution failure.

The earlier graphical validation covered rendering and window movement,
not an interactive Display preview on an unbound physical GPU or this
exact Doom launch. Those failures remain open. The
[VBE 3.0 mode/access-model specification](https://pdos.csail.mit.edu/6.828/2011/readings/hardware/vbe3.pdf)
and [S3VBEFIX documentation](https://github.com/wbcbz7/S3VBEFIX/blob/master/README.TXT)
are the references for comparing selectable modes with the actual V86
banked rendering path before further display changes.

Two additional read-only-source full-HDD runs used private image copies,
512 MiB / Pentium III / KVM / standard VGA, sequentially inside the same
3 GiB memory / 1 GiB swap scope. The separate `APPS/DOOM/DOOM.COM` audio
wrapper reported `EXEC FAIL`; it is not the TestGames/desktop core route.
Direct `DOOMCORE.EXE -warp 1 1 -nomusic` reached `V_Init: allocate screens`
but did not reach `ST_Init` within 120 seconds. The saved protected-mode
frame repeats at linear EIP 0024B84F: a `jnz` loop compares a global dword
at 002A013C (zero in the snapshot) with EAX=1Eh. This establishes a stuck
wait in that QEMU run. Later CR3-translated memory inspection identified
it as the error-reporting wait after a failed 256,000-byte conventional
allocation: only 176,112 contiguous bytes were available while the inactive
root shell retained 294,656 bytes. This does not establish the cause of the
physical V86 exception and does not justify changing IRQ delivery. The
bounded reclaim and global AH=48h gap scan are recorded in
[the VMFORK memory note](2026-10-07-vmfork-memory.md).
Reports, serial logs, screenshots and RAM captures are in
`doom-physical-profile-std/` and `doom-physical-profile-core/` under the
evidence directory's parent. No production code, image or physical disk
was changed during this investigation; the interactive Cirrus preview
and exact TestGames route had not yet been validated at that point.

The rebuilt canonical image subsequently reached `ST_Init` and rendered the
original DOOM title in its actual DOS window on 512 MiB / Pentium III / KVM
standard VGA (`doom-global-canonical/report.json`, `physical_hardware_qualified`
is false). Kernel, LFN, CVSESSION and VMFORK were rebuilt together; deliberately
mixing an older extension with the new kernel correctly failed its build-ID
check. Wrapper launch, gameplay, return and repeated launch still require
their separate runtime gate.

## Display followup

The properties app previously treated VBE ModeAttributes D7 (linear-framebuffer
availability) as the mode's access model and used the VBE 3.0 linear pitch
whenever it was nonzero. That disagreed with the actual renderer: when CR0.PE is
set, the shared VBE console cannot open its local LFB mapping and falls back to
banked writes. The runtime accepts banked access only when window A is
relocatable and writable, its segment is either unspecified or A000h, the
window is at least 64 KiB, and the granularity divides 64 KiB. The prior probe
could therefore list a linear-only mode that the active V86 renderer could
not use, or report the linear stride for a mode that would be rendered with a
different banked stride.

The Display probe now reads SMSW to match the renderer's PE gate. In V86 it
offers only modes with a validated banked path and uses the banked pitch and
RGB masks; in real mode it prefers a validated linear path and falls back to
banked metadata when linear layout validation fails. Banked and linear RGB
descriptors are validated separately. Current scanout reporting is independent
of candidate eligibility: it preserves the active 4F03 D14 access bit and
uses 4F06 BL=01's returned byte pitch when valid, so an active LFB is still
reported even if 4F01 availability bits are incomplete. The memory-fit check
now rounds by the VBE controller's 64 KiB TotalMemory units.

The VBE facts come from the [VBE Core Functions Standard 3.0](https://courses.cs.washington.edu/courses/cse451/20wi/readings/vbe3.pdf):
Function 01's mode attributes define D6 as windowed availability and D7 as
LFB availability (spec pp. 32–33); WinA attributes, size, segment, and
granularity describe the window (p. 30); VBE 3.0 gives separate banked and
linear pitch and RGB fields (pp. 38–39); Function 03 D14 identifies the
current access model (p. 44); Function 06 BL=01 returns BX bytes per scanline
(p. 48); and controller TotalMemory is measured in 64 KiB blocks (p. 26).
The [Intel SDM SMSW entry](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf)
states that SMSW returns CR0[15:0] and is available to application code when
UMIP is clear; this project targets Pentium III-class hardware, predating
UMIP. The implementation follows the runtime's existing PE check rather than
inferring that every DOS session implies V86.

`scripts/test_display_probe.py` runs the production `disp_probe_init` and
`add_mode` path against controlled BIOS mode-info replies. It checks that a
linear-only mode is omitted in V86, banked and linear pitches/masks remain
distinct, an invalid linear mask falls back to a valid banked descriptor, and
active LFB readback uses the 4F06 pitch despite a cleared 4F01 D7 bit.

DISPLAY.APP appends event-only ASCII hex records to `\SYSTEM\DISPLAY.LOG`.
The log is best-effort, capped at 4 KiB, and does not change the DOS drive or
current directory. Records capture the probe access path, selected/current
mode, native backend/phase/DOS-session guard, mode attributes, both window
descriptors, pitches, framebuffer address, RGB masks, 4F03 mode readback,
4F06 pitch readback, and preview/rollback outcome. It intentionally emits no
per-poll records. No physical S3 preview or resolution-change result has been
verified as part of this followup; the diagnostics are intended to capture
that evidence on the next user-run attempt.

## Files and Desktop associations followup

CiukiOS now opens BMP, PNG, JPG, JPEG, and GIF files with Image Viewer, and WAV,
MP3, OGG, and FLAC files with Music Player from Files and Desktop. Files keeps
the explicit “Open with CiukNote” action, and its Edit context action continues
to open BMP files in CiukPaint. PCX, MIDI/MID, VOC, and PCM retain distinct
unsupported type labels; matching an extension does not claim that CiukiOS has
a decoder for those formats. These lists follow the actual handlers in
`src/apps/viewer.c` and `src/media/decoder.c`. Microsoft's file-association
documentation describes the corresponding shell concepts: an extension
selects an open application, type label, icon, and context actions
([File Associations](https://learn.microsoft.com/en-us/windows/win32/shell/fa-how-work)).
CiukiOS keeps this mapping in its own Files and Desktop code rather than
claiming to implement the Windows registry model.

Files now draws image and music glyphs in list rows and uses the image/music
family icons for large views and Properties. The ten-line removable-media
preview and 172-entry directory limit remain intact. The media request packet
and background-job recursion paths now live in DOS-owned memory; the Files
stack remains 4 KiB. The complete application packaging script passed,
producing FILES.APP at 62,384 bytes and DESKTOP.APP at 60,096 bytes of required
memory. VIEWER.APP and PLAYER.APP also linked successfully. No full image or
QEMU run was performed for this followup.
