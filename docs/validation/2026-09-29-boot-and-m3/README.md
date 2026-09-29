# Boot without text, and Phase 2 M3 (DOS window sessions in their own VMs) — 29 September 2026

All evidence here comes from one image,
`build/full/ciukios-full.img` (SHA-256
`42f006a3a6461230f7be574aaf9dbb2e6830290d822610b0973c9a8c75dd10cc`), built by a
plain `scripts/build_full.sh`. It was tested on QEMU with KVM. Nothing here
qualifies physical hardware.

## 1. Boot: splash, then the desktop with the startup melody

Owner request: no text output during startup. Either the splash or the
desktop is on screen while the startup sound plays.

What showed text before:
- The loader left the splash for text mode at its last step, waiting 2 s.
- The kernel set mode 03h before it ran SHELL.
- SHELL set its text profile at startup.
- VMSTART, Jemm386 and JLOAD printed on that screen.
- BOOTSND then played the melody on it before the desktop's video began.

Now:
- **Kernel** (`src/boot/floppy_stage1.asm`).
  - The completed splash stays up. The 2 s wait is gone.
  - The first exec (SHELL at boot) does not set mode 03h. Later execs still
    do.
- **SHELL.**
  - No text profile at startup. The DOS-console boot choice still gets text
    mode through `vc_end`.
  - `ui_video_begin` tears down only an active console, so the desktop's
    VBE mode set replaces the splash directly.
  - The melody is the event sound driver's event 7, posted at desktop entry.
- **SFX.DRV** (`src/com/ui_sound_driver.inc`).
  - Event 7 plays `\SYSTEM\BOOT.PCM` asynchronously on the ICH: up to 32
    descriptors of 16 KB, in a block freed when the melody ends.
  - Without an ICH (Sound Blaster, PC speaker) it runs `BOOTSND.COM /Q` as
    before.
  - The `BOOT.SND` preference, and the SAFE and MUTE boot choices, still
    silence it.
- **Quiet programs** (`src/com/quiet_console.inc`).
  - On a graphics screen, VMSTART and the new `\SYSTEM\VIDEO\AUXSTART.COM`
    (which runs AUXSTACK) route console output to COM1.
  - The route is an INT 21h hook written into the IVT while their children
    run. CiukiDOS draws AH=02h/09h output whatever handle 1 is, and refuses
    AH=25h for its INT 21h vector. The boot log on COM1 keeps every message.
- **Restored VBE DOS helpers.** AUXSTACK, AUXCHECK, VIDMODES and MODETEST
  from the pinned vbesvga.drv release are back in `\SYSTEM\VIDEO`:
  - the Windows 3.1 removal had dropped them;
  - the desktop and VGASETUP use them;
  - the Win16 driver itself is not shipped.

Evidence (screendump every 0.1 s from power-on, `boot-frames.json`):
- **Frames.** BIOS, then the splash (800×600, full bar, no text:
  `boot-splash-last.png`), then the desktop (1280×800,
  `boot-desktop-first.png`). No text-mode frame.
- **Timing.** The desktop appears about 1.4 s after the splash, instead of
  5 s.
- **Sound.** `boot-audio.txt`: the melody was recorded (231 060 bytes, AC RMS
  3496).
- **Serial.** `boot-serial-excerpt.log`: the AUXSTACK, VMSTART, Jemm386 and
  JLOAD messages, on COM1 only.
- **Firmware fixture.** `scripts/qemu_test_startup_stack.py` passes with the
  PCI fault (`startup-stack-pci.json`) and without faults
  (`startup-stack-none.json`): desktop ready, startup audio, DOS console and
  return.

## 2. Phase 2 M3, first part: the DOS window session in its own VM

This covers the first part of M3. Sections 4 and 5 cover the rest.

Design: [record](../../design-multi-vm-2026-09-28.md). Gate:
`scripts/qemu_test_vmm.py`, part 4 (`VMWTEST`/`VMWCHILD`), in the profile as
`multi-vm` (`multi-vm.json`):
- VM 1's session and screen work without touching the physical display.
- Its keys were exactly a, b, c and the Esc sent through its device model.
- The system VM read its own x.
- VM 1 ended with exit code 0.
- Parts 1-3 (M1, M2) passed in the same run.

## 3. Complete profile

`scripts/test_vm_window_profile.sh --image build/full/ciukios-full.img
--output build/tests/vm-window-profile-2026-09-29a`: **21/21 pass**
(`profile-summary.json`, artifact hashes in `artifacts.sha256`). The profile
also passed as `...28f` (M3 only) and `...28g` (M3 and the first boot changes).

| Probe | SHA-256 |
| --- | --- |
| `VMWTEST.COM` | `5e7581f5634616fad6775f3bb99e767905be7de6a5d9a866ab5a6d9a9617238e` |
| `VMWCHILD.COM` | `378ed03149c843b470504864f44d87b5913d9fe70be3bd408182a37e87e498f7` |
| `AUXSTART.COM` | `3a86b0ff414ca527810b968317ce5e188267576fd2867f34c4b059762dd1c794` |
| `VMSTART.COM` | `4a05f63394a70da4dc5972634cf48311f7ddbe264246a990a91533a30b7ba476` |
| `SFX.DRV` | `55cd786c31d510a581396d2c5e39972ec61fbc632025f8c72cf1bb078581b374` |

## Open: disk-BIOS stack fixture

`qemu_test_startup_stack.py --fault disk` fails. This fixture makes every
INT 13h use 1 KB of the caller's stack, as documented firmware may.
- **This image** (`startup-stack-disk.json`, `failure-startup-stack-disk.png`):
  the kernel cannot load SHELL.COM (DOS error 8).
- **The previous build** (same kernel, other file layout,
  `failure-startup-stack-disk-earlier-layout.json`): it hung in JLOAD after
  Jemm386 loaded.
- **The phase 1 image of 28 September:** it also hung in JLOAD.

So a stack in the disk path has less than 1 KB free when the fixture is on,
and where it breaks depends on the layout. JLOAD's own stack is 1 KB. The
failure predates today's changes and is not in the complete profile.

## 4. M3 items since (29 September, later)

Image `build/full/ciukios-full.img` SHA-256
`2c29fdb99fb5bd306a5712cd624e334c4bddb4a4dadb0848005ac04ca91938d9`. Complete
profile `build/tests/vm-window-profile-2026-09-29b`: **21/21 pass**
(`profile-b-summary.json`, `profile-b-artifacts.sha256`). The `multi-vm`
gate now has seven parts, all passing in one boot (`multi-vm-7-parts.json`):

| Part | What it proves | Result |
| --- | --- | --- |
| switch | pre-emption, key wait inside DOS | pass |
| io | two VMs writing files, FAT clean | pass |
| window | session owned by VM 1, keyboard focus, Ctrl+Esc back to the system VM | pass: VM 1 keys a, b, c, Esc only |
| sound | SB16 DMA of VM 1 read while the system VM runs | pass: 220 Hz square wave, 100 % on level |
| kill | VM exits with its session open / ends it / hangs and is killed | pass: session free, next key intact each time |
| ivt | vectors pointing into blocks freed in a forked VM are restored | pass |
| clock | two busy VMs keep real time; per-VM CMOS index | pass: 183 and 181 ticks in 10 s |

Checks that the gates catch the defects they cover, on the same probes:
- `sound-before-fix.json`: the device model without the fix, 22 % on level,
  1000 crossings/s.
- `ivt-before-fix.json`: the previous VMFORK, "a vector still points into
  the freed block".
- `clock-without-delivery.json`: 91 and 91 ticks.

At that point several VMs with a session at once, mouse routing and audio
ownership were still missing. Section 5 covers them.

## 5. M3 complete: several sessions, mouse routing, audio ownership

Image `build/full/ciukios-full.img` (copy `build/tests/vm-mvm/image-m3.img`),
SHA-256 `6550a6dbee7e00971b64c5e468561f8af7348a6b415a5bd0209e57bf16f64b83`,
built by a plain `scripts/build_full.sh`.

Complete profile `build/tests/vm-window-profile-2026-09-29d`: **21/21 pass**
(`profile-d-summary.json`, `profile-d-artifacts.sha256`). The `multi-vm`
gate has ten parts, all passing in one boot (`multi-vm-10-parts.json`):

| Part | What it proves | Result |
| --- | --- | --- |
| switch, io, window, sound, kill, ivt | as in section 4 | pass |
| clock | two busy VMs keep real time | pass: 180 and 184 ticks in 10 s |
| multi | two VMs, each with its own session (VGA and device model) | pass: VM 1 keys a, Esc; VM 2 keys b, Esc; no VM text on the physical screen |
| mouse | the mouse follows the focus; DEV_MOUSE reaches the addressed session | pass: VM 1 X +12 then +5/+7 with the left button, VM 2 Y -12 only |
| audio | the AC'97 stream plays the focus VM's session (else the first that plays); the others play muted in real time | pass: each 3 s state is the owner's wave only (439.8 or 1099.1 crossings/s, 100 % on level); blocks at 0.97 and 0.99 of real time |

What each fix changed, on the same probes (the first two logs are from
probes that also printed DI, the last hex word on a key line):
- `multi-before-bios-ram-fix.log`: before the per-VM BIOS RAM, VM 2 resumed
  from INT 16h with VM 1's DI (00A2) and wrote its b in VM 1's column.
- `multi-before-focus-queue-fix.log`: before focus loss kept the queued
  keys, the Esc sent to VM 1 was dropped when the focus moved to VM 2, and
  VM 1 never ended.
- `audio-before-muted-fix.json`: before muted sessions advanced from the
  shared audio poll, VM 2 played its blocks at 0.86 of real time.

| Artifact | SHA-256 |
| --- | --- |
| `CVSESSION.DLL` | `d26cd3a098b9cded1d68f7ec4a67480b8463f4a885718440c0a16dfbedce6ff9` |
| `VMDTEST.COM` | `fd729520346b72387325fedb5c4dbcf4d7715863909be06e711ac8f89e141ad3` |
| `VMWCHILD.COM` | `1d09fd609f273d4f5bd93e2cec458b5b2295fbbc306a70a4cc16310a649643e2` |
| `VMOTEST.COM` | `5045a7b00e059a462ff64168812db3c8d4b69a07add9aafa1c4a1c3fb471a7d5` |
| `VMOCHILD.COM` | `6f1fee189490dcf16aec0fb27a08433aa915ed44367424cd851cd2835c0473df` |
| `VMATEST.COM` | `3fd0160e5900baed00bb3e6f6d7b1f0eda8200e92993d572ba558e6de1cad270` |
| `VMSCHILD.COM` | `186981d987584715eddfebfd86ba7fe7b47746b1de71fdf1b89c0ff091e3b057` |

Still open, outside M3: the disk-BIOS stack fixture (above) and the kernel
AH=48h allocator bug (design record). QEMU/KVM evidence only.
