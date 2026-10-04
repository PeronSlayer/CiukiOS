# Armada E500: boot stops after the splash

The owner reports that the splash progress bar reaches the end, but the
desktop does not appear and Caps Lock does not respond. The laptop reportedly
has about 512 MB RAM. This is a physical-machine report, not a QEMU result.

## What has been checked

- The original PATA installation was written and read back successfully on
  2026-10-01. Its first 134,249,984 bytes matched SHA-256
  `85b0ab24aca907ccd3cb4e075e812d9707c0ea78b66bc5046c27c39ae1504aa4`.
  QEMU booted that image to `[DESKTOP] READY` with 128 MB RAM.
- A diagnostic `SHELL.COM` was built with `BOOT_DIAG_PALETTE=1`. It changes
  palette entry 255, used by the splash progress blocks, after each startup
  phase. The normal build is byte-for-byte identical to the previous shell.
  One QEMU boot of the diagnostic image reached `[DESKTOP] READY` at 128 MB.
- A fresh diagnostic image is prepared at
  `build/hardware/pata-2026-10-01/ciukios-pata-diagnostic-prefix.img`, SHA-256
  `0570d414cd726dfa6cf1245e210a51810b96d56051ae4f26c17f44f651043f50`.
  **It has not been written to the PATA disk.**
- Before any diagnostic write, a guarded read of the physical disk failed
  with `EIO`. The Linux log reports a 30-second USB read timeout at sector 0,
  further read errors, USB resets, and disconnection of the JMicron
  `152d:2338` USB-to-ATA bridge. The `/dev/sdc` block device then disappeared.
  The guarded writer stopped before its first write.

The physical failure could be the drive, the USB bridge, power, or cabling.
It does not prove which component is at fault or whether it caused the
Armada's boot stall. The prior E500 investigation is in
[`docs/e500-drivers-2026-09-26.md`](../../e500-drivers-2026-09-26.md).

## Next physical check

Reconnect the drive with reliable power and cabling. Confirm its exact serial
and that the read path is stable before trying the diagnostic image. Keep the
original first-128-MB backup in `build/hardware/pata-2026-10-01/`.

If the diagnostic image is installed and the splash stops again, photograph the
progress bar. Its last colour identifies the last completed shell phase:

| Colour | Last completed phase |
|---|---|
| Original white | Shell entry not reached, or no diagnostic palette update |
| Red | Shell entry |
| Orange | Auxiliary video stack |
| Yellow | Input, hardware detection and startup drivers |
| Green | Startup session choice |
| Cyan | Long file names |
| Blue | VM manager |
| Magenta | Hand-off to desktop |
| Pink | Desktop assets loaded |
| Lime | Desktop settings loaded |
| White after pink/lime | Sound driver returned |
| Grey | UI icons loaded |
| Teal | Mouse initialization returned |
| Sky blue | Video profile resolved |
| Amber | Calling VGA mode 12h |

The colour reports a boundary, not a final root cause. A normal desktop mode
set replaces the splash. Hardware success remains unverified.

## Replacement storage, 2026-10-01

The Toshiba USB-to-PATA path later failed to enumerate. A Fujitsu 80 GB disk
was inspected and backed up read-only, then the owner explicitly excluded it
from further work. No write was made to the Fujitsu.

The owner selected a Transcend `TS64GMSA230S` mSATA SSD in a PATA adapter,
serial `H551120730`, as the replacement. Its existing 96 MB `CIUKIOSFULL`
FAT16 volume had cluster-chain errors and was unmounted. The first
134,249,984 bytes were backed up read-only as
`build/hardware/transcend-2026-10-01/transcend-original-first-134m.img`,
SHA-256 `d06e5a6312cba95878e9396369edfb6a349b6c3a3303c7280e9c4e3b9a820bcf`.

A 64 GB sparse IDE image using the prepared diagnostic CiukiOS prefix reached
`[DESKTOP] READY` in one QEMU boot with 128 MB RAM. This checks the new disk
geometry in emulation; the Armada still needs a physical boot test. The
adapter reports no discard support, so the owner's full-format request uses
a sequential logical zero-fill before writing the 128 MB CiukiOS FAT16 boot
volume. The rest of the 64 GB disk remains unallocated because CiukiDOS's
system volume is currently FAT16. The zero-fill is not a cryptographic secure
erase of SSD remapped cells.

The owner explicitly authorized the format and CiukiOS install. The guarded
installer zero-filled all 64,023,257,088 logical bytes, wrote the diagnostic
CiukiOS prefix to sectors 0–262206, flushed it, and read back that prefix by
SHA-256: `0570d414cd726dfa6cf1245e210a51810b96d56051ae4f26c17f44f651043f50`.
The MiB after the installed prefix and the final MiB of the disk both read as
zero. Linux sees one active FAT16 `CIUKIOSFULL` partition at LBA 63, 128 MB.
No kernel I/O error occurred during the erase or readback. Machine-readable
receipt: `build/hardware/transcend-2026-10-01/physical-install-result.json`.

The source partition image passed `fsck.fat -n`, and the physical prefix
matched that source. A separate root-level `fsck.fat -n` against the physical
device was interrupted while waiting for Polkit authorization, so it provides
no additional direct-device result. The Armada E500 physical boot still needs
the owner's test. The splash colour table above applies to the installed
diagnostic image.

## First physical result and hardware mode

The first Transcend installation reached the splash on both physical
machines. The owner reported **magenta on the Compaq Armada E500** and
**cyan on the IBM ThinkPad T23**. Those boundaries place the Compaq after
VM manager startup but before the desktop replaces the splash, and place
the T23 inside VM manager startup. The exact subroutine has not yet been
proven. The VMSTART default memory map was validated on QEMU/SeaBIOS and
may conflict with real BIOS memory; VGA BIOS calls while the monitor is
resident are another plausible Compaq-specific cause.

The installed `SHELL.COM` now recognizes persistent `HARD` in
`\SYSTEM\STARTUP.CFG` on an installed C: volume. That mode skips the VM
manager and LFN TSR, forces VGA, and avoids startup audio. It leaves the
ordinary DOS console available while the physical monitor issue is
investigated. Diagnostic splash colours also cover the desktop entry path
in more detail. A single QEMU check of the prepared HARD image showed a
640×480 desktop and working DOS console; the owner then requested no more
QEMU testing for this hardware iteration.

The guarded update backed up the previous physical first 134,249,984 bytes
to `build/hardware/transcend-2026-10-01/transcend-before-hard-prefix.img`
(SHA-256 `d714356424d4866b10314ac76a31277bfebbc589e0b6be748ad28c7706341f90`),
wrote the HARD image, and read back the same SHA-256 as the source:
`d3536ac16d762c666fc9dd2ab947af80dd3f11ea8366c0dcdc50cb174677c4b9`.
Physical HARD-mode results reported by the owner:

- Armada E500: the 640×480 desktop appears, but neither keyboard nor mouse
  responds. This is a physical observation; the cause is still unknown.
- ThinkPad T23: one attempt displayed the BIOS message “Operating system not
  found”. A subsequent attempt reached the desktop, where keyboard and mouse
  work but the screen visibly redraws/flashes. The owner provided
  `docs/IMG_3108.MOV`, an 8-second video of the T23 desktop. Thus the BIOS
  message was intermittent, not evidence that the written MBR was always bad.

The MBR, active partition table, and the first 63 sectors of the HARD image
were byte-for-byte identical to the earlier image that reached the splash on
both laptops. The physical backup taken after the owner's Compaq and T23
tests still has the same first 63 sectors, including the `55 AA` boot
signature and active partition at LBA 63. The complete HARD prefix also
matched its physical readback hash immediately after installation. This does
not rule out an intermittent disk/adapter/BIOS problem.

## Prepared persistent boot log

`scripts/prepare_physical_bootlog.py` prepares a new image from the HARD
prefix. The image is
`build/hardware/transcend-2026-10-01/ciukios-bootlog-prefix.img`, SHA-256
`1a60cc55132ee2eac3e6ab891f6e6074713d1ff1f4248f0b25d32e522158effe`.
It keeps the active FAT16 partition at LBA 63 and the HARD startup choice.
`fsck.fat -n` reports 883 files and no filesystem errors. No QEMU test was
run, at the owner's request.

The guarded installer wrote that image to the identified Transcend SSD and
read back all 134,249,984 bytes with the same SHA-256. The previous physical
prefix was saved as
`build/hardware/transcend-2026-10-01/transcend-before-bootlog-prefix.img`,
SHA-256 `a82e181d2dc40384499ed5f02645df63909fc64b826986d727a6c8f662a5df53`.
The receipt is
`build/hardware/transcend-2026-10-01/physical-bootlog-install-result.json`.
The SSD was then safely powered off for the owner's physical test. The owner
booted the IBM T23 and then the Compaq E500 and reconnected it. The read-only
log extraction is saved in `physical-bootlog-first.txt`.

Both MBR records reach stage `0x05` (CiukiDOS validated). Both shell runs
reach `S10` (first desktop frame painted). The IBM run also has `S12` (first
mouse motion); the Compaq run has no input marker. Neither run repeats the
video-initialization markers. The logged evidence therefore rules out a
repeated mode set in those attempts. The VGA fallback's direct painter draws
the desktop over the visible screen on each repaint; the supplied video shows
a partially repainted Control Panel frame. The next diagnostic build composes
VGA frames in RAM bands and adds one-time UI-loop, HLT and mouse-reset markers.

That second diagnostic image is
`build/hardware/transcend-2026-10-01/ciukios-vga-bootlog-prefix.img`, SHA-256
`8ac9c1d8d10936250f43a4b476c04d4f7ff4c53a31a25bc5476d153e53ca4224`.
It allocates a 32 KiB conventional-memory band in VGA mode, renders 16-color
indices there, and writes completed bands to VGA's four bit planes. A failed
allocation falls back to the previous direct path. `S19` records successful
allocation; `S1A` records failure. `S13`/`S14` bracket the first input poll,
`S15`/`S18` bracket the first idle HLT, and `S16`/`S17` report whether the
mouse reset succeeded. The FAT16 image passed `fsck.fat -n` after this update;
the diagnostic SHELL.COM is 59,664 bytes, within its size bound. No QEMU test
was run, at the owner's request.

The second image was written to the same serial-pinned Transcend SSD and its
134,249,984-byte prefix read back with the same SHA-256. The former physical
prefix, including the two captured boot logs, was saved as
`build/hardware/transcend-2026-10-01/transcend-before-vga-prefix.img`, SHA-256
`204c25d808edab9551aa429fd554687a2f817203b237b58f8efa405e80d216d7`.
The receipt is
`build/hardware/transcend-2026-10-01/physical-vga-install-result.json`.
The SSD was safely powered off for physical tests; boot behavior is unverified.

The owner then tested IBM followed by Compaq. Both reached the desktop, but
the new VGA band image corrupted text and window contents on both laptops;
the IBM flashed roughly once every 30 seconds. The IBM mouse and keyboard
worked. On the Compaq, the built-in touchpad and keyboard still did not.
Photos: `docs/photo_2026-10-01_18-02-38.jpg` and
`docs/photo_2026-10-01_18-06-20.jpg`. The extracted marker sequence is in
`physical-bootlog-vga.txt`: both machines have `S16` (INT 33h ready), `S19`
(band allocated), `S10` (first frame), `S13`/`S14` (input poll returned), and
`S15`/`S18` (HLT woke). The IBM also has `S12` (mouse motion); the Compaq does
not. The Compaq's desktop loop is alive. The experimental VGA band renderer
was withdrawn from the physical build; it needs a separate correctness fix.

The next input-diagnostic image restores the prior direct VGA rendering and
adds a legacy INT 16h keyboard fallback in HARD mode, plus one-time PIC IRQ1,
PIC IRQ12, and INT 33h ownership markers. It is
`build/hardware/transcend-2026-10-01/ciukios-inputdiag-prefix.img`, SHA-256
`cd319eedaf5a7d3d2a98784a135911650c589a3fd0996b1ec9ccfcbe82782724`.
The partition passed `fsck.fat -n`; no QEMU was run. The serial-pinned SSD
was written and its prefix read back with the same hash. The previous physical
prefix, including the IBM/Compaq logs, was backed up as
`build/hardware/transcend-2026-10-01/transcend-before-inputdiag-prefix.img`,
SHA-256 `ae5314404becb8dc9e24c6b43f17819cbaa9584f1d670a48af0a11a0f51b6d8d`.
Receipt: `build/hardware/transcend-2026-10-01/physical-inputdiag-install-result.json`.
The SSD was safely powered off for a Compaq E500 test. The owner reports that
the desktop starts, the keyboard works, and the built-in TouchPad moves the
pointer and clicks when tapped. Its separate physical buttons do not work.
Changing resolution has no effect because `HARD` deliberately forces the
640×480 VGA fallback. On this third image the drawing is correct, but the
screen still visibly flashes during redraws.

The read-only extraction after that test is in
`physical-bootlog-inputdiag.txt`. It reaches `S10` (desktop), then records
`S1C` (IRQ1 unmasked), `S1E` (IRQ12 unmasked), `S20` (CiukiDOS owns INT 33h),
`S12` (mouse motion) and `S11` (key read). There is no `S1B` marker, so the
legacy BIOS keyboard fallback was not used. Repeated `S07`–`S10` sequences
follow the owner's attempts to change display settings. The earlier loss of
all input coincided with the experimental VGA band renderer; the evidence
does not isolate its exact fault. Button hardware versus packet decoding
also remains unproven.

The [Armada E500 Technical Reference Guide](https://mail.jamesthompsononline.com/pics/2017docs/Computer%20Notes/CompaqE500/armadaE500_TechRefGuide_11qy-0200a-wwen.pdf)
describes the TouchPad and its physical buttons as inputs to the PS/2 pointing
device. The [Synaptics Interfacing Guide](https://ptacts.uspto.gov/ptacts/public-informations/petitions/1482694/download-documents?artifactId=SgAdRVgXupA6vBdn0lpJnNBoDwdoquj1fujHL8)
specifies standard three-byte relative packets by default and button-change
packets. CiukiDOS decodes those three button bits; the existing boot log does
not show whether button presses reach that decoder. The next physical image
therefore includes a read-only mouse diagnostic: it counts complete IRQ12
packets and raw left/right press edges, so a tap and each physical switch can
be compared without guessing at a firmware or hardware cause.

## VGA redraw follow-up

The direct VGA path repainted the entire visible desktop when the desktop
module updated the top indicators. `EV_PAINT_TOPBAR` now paints only those
indicators; the shell redraws only its top rail when the pending damage lies
entirely within the top 29 pixels. Other damage continues through the
existing full redraw path. The normal full-profile image builds, and the
physical prefix passes `fsck.fat -n`; neither result establishes visual
correctness on the two laptops.

Prepared physical image:
`build/hardware/transcend-2026-10-01/ciukios-vga-topbar-prefix.img`, SHA-256
`1018a88e21c9cbcebe76d19533c5ef2c6090e29143ca11f47f02311e158b4f47`.
The logged `SHELL.COM` is 59,552 bytes, under the hard size ceiling. No QEMU
run was made for this hardware iteration.

The MBR keeps four records in otherwise unused raw sectors LBA 1–4. Each
boot advances one slot; the MBR, partition boot sector and Stage1 update its
last completed stage. These writes use BIOS CHS sector 2–5, never FAT16, and
are best effort. `scripts/read_physical_bootlog.py <whole-disk device>`
reads them without writing. Their stage numbers are documented in the
reader. If the BIOS never executes the MBR, no code on the disk can record
that attempt. A missing record can also mean that BIOS writes failed.

Once DOS starts, the diagnostic shell appends `Sxx` phase records to
`C:\SYSTEM\BOOT.LOG`, closing the file after each record. `S10` means the
first desktop frame was painted; `S11` is the first key, `S12` the first
mouse motion. Repeated `S0A`–`S0D` after `S10` would show repeated video
reinitialization. The log must be read from the SSD after each machine is
tested; the reader cannot infer the machine model from the BIOS drive number.

## Structural VGA renderer work

The main source now composes Mode 12h desktop bands as indexed pixels in RAM
and converts each completed band to VGA's four planes. Damage is widened to
whole eight-pixel groups for planar writes. Rectangles, icons, Ciuki's portrait
and text use the scratch path during composition. The earlier experiment's
corrupt text was traced to the glyph dispatcher still writing directly to
VGA; that dispatch has been corrected.

The full HDD build and FAT16 check pass. CPU tests verify clipped drawing and
the exact bytes written to all four VGA planes. QEMU booted a disposable copy
of the full HDD with `STARTUP.CFG=HARD`; desktop and About screenshots show
correct text and artwork, and the full HDD smoke test reached desktop
readiness. These results do not establish the behavior on either laptop. The
new build has **not** been installed on the Transcend SSD in this step.
Diagnostic builds record `S19` when the new VGA scratch band is allocated and
`S1A` when allocation falls back to direct painting.

The current renderer architecture and its remaining limits are described in
[`docs/gui-rendering-architecture.md`](../../gui-rendering-architecture.md).
