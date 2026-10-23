# Quake shareware pre-installation (F3-18)

Research checked 2026-10-14, before implementation. The pinned Quake 1.06
DOS shareware distribution is installed whole and byte-identical under
`\QUAKE`, including `ID1\PAK0.PAK`, its DPMI fallback and Windows helpers.
`SLICNSE.TXT` sections 2/3/6 permit free electronic distribution of the whole,
unmodified shareware package; both licence documents accompany the system
image. These terms concern the original binary/data, independently of the
later GPL engine source. No commercial Quake data is included.

Sources and decisions:

- [Debian game-data-packager](https://sources.debian.org/src/game-data-packager/49.1/data/quake.yaml/)
  and its [hash addition](https://alioth-lists-archive.debian.net/pipermail/pkg-games-commits/2015-January/023633.html)
  pin quake106.zip and pak0.pak; the independent
  [Dreamcast Quake archive](https://sourceforge.net/projects/dcquake/files/quake/shareware/quake106.zip/download)
  publishes the matching archive SHA-256. Keep the original archive locally;
  builds verify the pin and never download anything.
- [LHa header specification](https://github.com/jca02266/lha/blob/master/header.doc.md),
  [header.c](https://github.com/jca02266/lha/blob/master/src/header.c),
  [slide.c](https://github.com/jca02266/lha/blob/master/src/slide.c),
  [huf.c](https://github.com/jca02266/lha/blob/master/src/huf.c),
  [Lhasa lh5 decoder](https://github.com/fragglet/lhasa/blob/master/lib/lh5_decoder.c),
  [new-style decoder](https://github.com/fragglet/lhasa/blob/master/lib/lh_new_decoder.c)
  and [reader](https://github.com/fragglet/lhasa/blob/master/lib/lha_reader.c):
  implement bounded MSB-first static Huffman/LZSS decoding in Python with an
  8192-byte space-initialized dictionary, lengths 3..256, member CRC-16
  (reflected polynomial 0xA001, initial zero), and level 0/1/2 headers.
  Reject bad checksums, trees, lengths, truncation, duplicate DOS paths and
  traversal. The vendored resource.1 is an MZ LHA self-extractor: its first
  member header is at byte 1945. Pin that offset explicitly in UPSTREAM.json.
- [id Software DOS system code](https://github.com/id-Software/Quake/blob/master/WinQuake/sys_dos.c)
  uses DJGPP/DPMI memory information and locking, and x87 floating point.
  Check the bundled TECHINFO.TXT requirements against our runtime; use the
  existing native DPMI server rather than starting another protected-mode
  host inside the VM.
- [id Software DOS audio code](https://github.com/id-Software/Quake/blob/master/WinQuake/snd_dos.c)
  selects high DMA for SB16, emits DSP 0xB6 with mode 0x30 (signed 16-bit
  stereo auto-init), and polls the DMA position while acknowledging 16-bit completions at
  base+0x0F. The existing dosvm_sb16.c
  B0..CF path decodes this format and selects DMA5; dosvm_dma.c implements
  the second 8237 controller. Retain `BLASTER=A220 I7 D1 H5 T6`.

- [FAT directory format](https://elm-chan.org/docs/fat_e.html) (FatFs upstream):
  extend the host FAT16 writer to approved nested directories; dot points to
  the current directory and dot-dot to its actual parent (zero for root).

The existing FAT16 games volume has 128 MiB logical capacity. Its compact
file retains allocated sectors only; the additional 18.7 MB PAK fits without
changing geometry or the 512 MiB outer-image budget. Build T1 read-back and
manifest hashes must cover all 16 files, nested directories and licences.
The launcher uses the established `input-gaming` Tango icon and types
`CD \QUAKE` then `QUAKE.EXE` through autorun.h.

Host checks cannot qualify gameplay. The lead must build the canonical
image, run `quake-qemu-t23`, and check the desktop launch, input, sound and
exit on the T23. QEMU evidence remains tied to that image's SHA-256.

Runtime comparison: bundled README.TXT requires a Pentium/FPU, VGA, 8 MB RAM
(16 MB recommended), DOS 5+ or Windows 95, and 30 MB shareware disk space.
CiukiOS window VMs commit 32 MiB, with 8 MiB reserved for low/device support
and a 24 MiB shared XMS/EMS/DPMI pool; no memory limit changes are necessary.
INT 2Fh/1687h advertises the staged native DPMI host for game windows; there
is no HDPMI32 in dos_fixtures.py. CWSDPMI is retained as original package
content, but the native host takes precedence. x87 uses the existing guest
FPU context. BIOS VGA mode 13h and INT 33h are available. Guest VBE/SVGA and
CD audio are not implemented; the host BIOS VBE framebuffer does not expose
VBE to DOS clients. Stick to the default 320x200 mode and SB16 PCM.

The native probe uses the bundled base directly (no owner-local fixture),
records the hashes/sizes of all 16 files, and starts `+playdemo demo1`.
Quake's graphics console cannot be parsed through the B8000 text cells as
DOOM's exit timing can. Require 32 changing 320x200 frame samples over at
least 10 seconds, active DPMI with >=16 MiB quota, SB16 16-bit stereo
auto-init DMA5 with >=4096 frames/four blocks, then console `quit` through
8042 set-1 events, RUNREC normal exit and complete DPMI/VM cleanup. The
changing-frame threshold is a smoke gate, not a pinned FPS benchmark or
a substitute for visually confirming the demo on QEMU/T23.

The original licence text refines the directive's section references:
section 2 prohibits commercial distribution, section 3 modification, and
section 6 permits free whole-package distribution by end users. Providers
have an additional compressed-format condition. Preserve the original ZIP
and complete licence text; all release policy decisions remain with the lead.

## CD audio (fx-10)

Research checked 2026-10-23, after the T23 trial of preview `423b803a`
(sound effects, no music).

Sources:

- id Software [WinQuake cd_audio.c](https://github.com/id-Software/Quake/blob/master/WinQuake/cd_audio.c)
  and [dos_v2.c](https://github.com/id-Software/Quake/blob/master/WinQuake/dos_v2.c)
  (GPL release): `CDAudio_Init` issues INT 2Fh AX=1500h with BX=0 through
  `dos_int86`, i.e. DPMI 0300h (`_go32_dpmi_simulate_int`), and disables
  music with "MSCDEX not loaded" when BX is still 0; then 150Ch, IOCTL output
  3 (channel volume, twice), IOCTL input 10/11 (disk and track info), PLAY
  AUDIO 132 in Red Book, IOCTL input 15 every 0.25 s while the busy bit is
  set, STOP AUDIO 133 as pause and RESUME AUDIO 136.
- Microsoft [MS-DOS CD-ROM Extensions 2.1](https://data.phys.ucalgary.ca/sort_by_instrument/other/poca/eu9900/005/IN2000/CDROM/MSCDEX21.DOC)
  (request header and status word, busy bit while playing, IOCTL control
  blocks, Red Book `Sector = Minute*4500 + Second*75 + Frame - 150`, the
  PLAY/STOP/RESUME state machine) and the
  [Function Requests Specification](https://cassini.mirrorservice.org/sites/ftp.cdrom.com/pub/cdrom/textinfo/mscdfunc.doc)
  (INT 2Fh AX=1500h..1510h).
- Ralf Brown's Interrupt List, [INT 2Fh/1510h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4493.htm)
  and [INT 21h/4402h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-2829.htm)
  table 01450: device status bit 10 "audio is playing", bit 11 "no disk in drive".
- FreeDOS kernel 2043 `kernel/int2f.asm` (vendored): the INT 2Fh dispatcher
  has cases for AH=08h, 10h-14h, 16h, 46h, 4Ah and IRETs otherwise.

Root cause: no CiukiOS layer answered INT 2Fh AH=15h. The monitor reflected
Quake's 0300h call to the guest's FreeDOS handler, which IRETs with BX=0, so
Quake never asked for a track. f5-12B2 had built the kernel CD-DA transport
and the Win32 `cdaudio` MCI device, but explicitly left DOS MSCDEX to F3; the
shareware archive also contains no music (its CD tracks are commercial).

Decision (Windows 98 model: the VMM answers MSCDEX for its V86 VMs):

- `src/kernel/vm/dosvm_mscdex.c` models MSCDEX 2.23 and one CD-ROM drive per
  window VM: 1500h, 150Bh, 150Ch, 150Dh, 1502h-1509h refusals and 1510h with
  IOCTL input 1/4/6/7/8/9/10/11/12/15, IOCTL output 0/1/2/3/5, PLAY, STOP,
  RESUME, SEEK, open/close/flush; READ LONG answers "sector not found" on an
  audio-only disc. 1501h (device header list) is not offered: no guest-
  resident driver exists, so programs that call a CD driver directly are not
  supported. The drive letter is the first after the VM's DOS drives (F: on
  the unified image). The head position runs from the play clock at 75
  sectors per second.
- `src/kernel/vm/dosvm_cdaudio.c` answers both entries before the guest chain
  (`dosvm.c` V86 INT 2Fh and `dpmi_switch.c` 0300h, the latter returning
  through the real-mode return stub as `dosvm_file_reflect` does).
- The disc is virtual. Tracks are RIFF/WAVE CD-DA files (PCM, 44.1 kHz,
  16-bit stereo, the Red Book sample format) in
  `C:\GAMES\CDAUDIO\<TITLE>\TRACKnn.WAV`, else `C:\GAMES\CDAUDIO\TRACKnn.WAV`;
  `<TITLE>` is the launched window's name up to its first space (Quake ->
  `QUAKE`). Track 1 is a 300-sector data track followed by the 2 s pregap
  unless `TRACK01.WAV` exists (mixed-mode game CDs; Quake's music is tracks
  2-11). Tracks are contiguous from 02; the first missing or refused file ends
  the disc. CiukiOS ships no tracks: only audio the owner has the right to
  use (for example a rip of a Quake CD the owner owns) belongs there.
- One kernel task, `vm-cdaudio`, scans the folder at VM launch, opens a native
  mixer stream per playing VM (44.1 kHz; the f4 mixer's Catmull-Rom path
  resamples to 48 kHz), reads 8 sectors at a time and feeds it every 10 ms
  while playing, with the gain of the MSCDEX channel volume and the VM's
  CT1745 CD (36h/37h) and master (30h/31h) levels. The VM worker never does
  file I/O or logging: requests and records are posted to the task. The VFS
  table is held only while a scan or a track file needs it.
- Records (`[dosvm] cdaudio ...`): `disc ... result=disc|no_tracks|storage`,
  refused track files with the reason, `mscdex ... drive= disc= result=`,
  `play/stop/resume/complete`, the first refusal per error code
  (`refused ... reason=no_tracks source=...` when the folder has no tracks),
  stream and read failures, and a `close` summary per VM.

Host evidence: `tests/host/vm/cdaudio_test.c` (Quake's request sequence over
0300h and V86, play/stop/resume, mixer routing through the production stream
and mixer, the missing-track record) and `tests/host/test_cdaudio_wiring.py`.
Not qualified by host execution: audible output, the T23 mixer level and
cadence under a running game. A physical CD drive is not used by this path;
routing the drive's own CD-DA (the f5-12B2 transport) to MSCDEX is a separate
step.
