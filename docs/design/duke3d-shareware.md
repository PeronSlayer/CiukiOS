# Duke Nukem 3D shareware pre-installation (fx-duke3d)

Research checked 2026-10-26 before implementation. Install only the original
3D Realms v1.3d episode-one release, `3dduke13.zip` (5,924,374 bytes,
SHA-256 `c67efd179022bc6d9bde54f404c707cbcbdc15423c20be72e277bc2bdddf3d0e`).
The existing `config/dos-titles.json` test-media pin names the same archive.
The implementer's sandbox cannot resolve download hosts; the byte-identical
archive was read from the lead's download cache, with its hash checked before
vendoring. Builds use only the vendored archive and never download.

Sources and decisions:

- The [3D Realms master download page, preserved by its FTP archive](https://ftp.gamefilearchive.com/ftp.3drealms.com/stuff/Downloads.html)
  identifies `3dduke13.zip` as the v1.3d shareware episode. Original URL:
  `ftp://ftp.3drealms.com/share/3dduke13.zip`; stable mirror of that publisher
  tree: <https://www.gamers.org/pub/mirrors/ftp.3drealms.com/share/3dduke13.zip>.
- The archive's own `DN3DSW13.SHR!LICENSE.TXT` (V.7.16.98) is the binding
  primary source: [3][A]/[B] allow no-charge individual sharing and free
  download services; [3][C] requires the complete unchanged release;
  [3][D] requires written permission and a fee/agreed benefit for other
  distribution, explicitly CD-ROM. Permission is not on file. The owner's
  request authorizes implementation, not a claim of publisher permission.
  Preserve all 17 installed members and the notice; record these conditions
  in the CD catalogue for the lead's publication decision. No retail GRP or
  executable is accepted, even under the same filename.
- 3D Realms' released [CONFIG.C](https://raw.githubusercontent.com/videogamepreservation/dukenukem3d/master/source/CONFIG.C)
  (`CONFIG_ReadSetup`, `CONFIG_ReadKeys`, `readsavenames`) supplies the section
  and key names, video parameters, and `GAME0.SAV` through `GAME9.SAV` naming.
  [SNDCARDS.H](https://raw.githubusercontent.com/videogamepreservation/dukenukem3d/master/source/SNDCARDS.H)
  assigns SoundBlaster=0 (PCM and OPL music), Adlib=3 and GenMidi=4.
  This is the later v1.5 source release, used as a format reference only;
  check the exact v1.3d executable's config reads in the host replay.
- Apogee's [BLASTER.C](https://raw.githubusercontent.com/videogamepreservation/dukenukem3d/master/audiolib/SOURCE/BLASTER.C)
  (`BLASTER_Init`) installs the configured IRQ handler and resets the DSP;
  it does not configure the SB16 IRQ-select register. CiukiOS's existing
  `dosvm_sb16.c` accepts mixer 80h writes selecting IRQ 5 and mixer 81h
  selecting DMA 1/5, but defaults to IRQ 7. A small original DOS helper
  programs those existing registers before Duke starts, and restores IRQ 7
  afterwards. Set BLASTER consistently in the batch. This avoids changing
  the global VM default or adding any kernel dispatch or panic path.

Install under `\\DUKE3D` with a generated `DUKE3D.CFG`, `DUKE3D.BAT` and
`DUKESB.COM`. Use 320x200 VGA, SB16 A220 I5 D1 H5 T6, stereo 16-bit PCM
at 22000 Hz and SoundBlaster OPL music. The release contains no ICO, PIF,
Windows icon library or NE/PE program: keep the existing Tango game icon.
Append game ID 5 to the catalogue, leaving IDs 1-4 stable. The existing
per-game COW saves, settings, export/reset and Add/Remove paths then apply
to Duke's CFG and SAV files through the ordinary DOS filesystem.

Host replay is evidence only for extender startup and the observed calls;
libx86emu lacks x87. Title screen, gameplay, audible PCM/OPL, exit and save
persistence need the lead's QEMU check documented in the implementation report.

## Host replay result

The production DOSX replay observes the bound DOS/4GW Professional 1.97
client entering DPMI and receiving protected-mode timer interrupts. The
launcher selects IRQ 5 and DMA 1/5 in the real virtual SB16 model.
With FPU detection enabled, execution reaches `D9 EE` (`FLDZ`) at
`01f7:00295687`; libx86emu has no x87 arithmetic and reports its existing
unsupported-instruction event. DOSX accepts only that exact measured
frontier for this pinned binary, prints `STARTUP ONLY`, and still fails
on any other unexpected event or any DOS/4GW error. It does not claim that
the CFG was read, VGA started, or sound played.

The initial experiment inherited Warcraft II's `GUEST_NO_X87=1`. Duke's
no-coprocessor branch attempted `0F 20` (a control-register read) at
`01f7:00299f62`, causing DOS/4GW error 2001 / exception 0Dh. Intel's
[SDM Vol. 2B, MOV to/from control registers](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf)
restricts that instruction to CPL 0. This is evidence about the artificial
no-x87 profile, not a reason to relax production control-register privilege
checks: all supported machines have x87. Keep Warcraft II's existing
no-coprocessor test and enable the runner's existing FPU detection for Duke.
The original binary and production monitor remain unchanged.
