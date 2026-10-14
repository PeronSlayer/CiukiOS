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
