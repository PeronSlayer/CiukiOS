# f3-18 — Quake shareware 1.06 pre-installed (owner request 2026-10-14: a free 3D game to test)

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f3-18` from `main`. Host-testable; the lead builds the image and runs the QEMU case.

## Pinned archive (downloaded by the lead on 2026-10-14)

`quake106.zip`, 9 094 045 bytes, SHA-256 `ec6c9d34b1ae0252ac0066045b6611a7919c2a0d78a3a66d9387a8f597553239`, SHA-1 `f8a1a509b094ccdbed3c54b96f7d9b351c0898f5`, MD5 `8cee4d03ee092909fdb6a4f84f0c1357`; byte-identical from `https://mirrors.lug.mtu.edu/idgames/idstuff/quake/quake106.zip`, `https://www.gamers.org/pub/idgames/idstuff/quake/quake106.zip` and `https://ftp.fu-berlin.de/pc/msdos/games/idgames/idstuff/quake/quake106.zip` (original location `ftp://ftp.idsoftware.com/idstuff/quake/quake106.zip`). A copy is already in this worktree at `third_party/games/quake-shareware/quake106.zip`. Contents: `deice.exe`, `install.bat`, `resource.dat`, `resource.1` = an LHA archive, 16 members, all method `-lh5-`: `QUAKE.EXE` 404480, `ID1/PAK0.PAK` 18689235, `CWSDPMI.EXE` 25920, `QLAUNCH.EXE`, `GENVXD.DLL`, `MGENVXD.VXD`, `QUAKEUDP.DLL`, `PDIPX.COM`, `Q95.BAT`, `HELP.TXT`, `LICINFO.TXT`, `ORDER.TXT`, `READV106.TXT`, `README.TXT`, `SLICNSE.TXT`, `TECHINFO.TXT`.

**Licence:** `SLICNSE.TXT` §6 "Permitted Distribution": the shareware version may be distributed free of charge, by electronic means, **as a whole and unmodified**; §4 forbids distribution for money and modification. Therefore install every member unmodified into `\QUAKE` (keep `ID1\PAK0.PAK` under its directory) and ship `SLICNSE.TXT` and `LICINFO.TXT` in `/system/licenses` as the DOOM and Wolf3D packages do.

## Task

1. **Package** `third_party/games/quake-shareware/`: the archive, `UPSTREAM.json` in the schema of `doom-shareware/UPSTREAM.json` (hashes above, the three mirrors, `downloaded 2026-10-14`, and at least two independent hash references found online, for example Debian `game-data-packager` `data/quake.yaml`, an AUR/RPM Fusion/Fedora shareware package, Quaddicted's file list), `README.TXT` provenance like the DOOM one, and the members' CRC-16 from the LHA headers as `member_references`.
2. **Pure-Python lh5 decoder** in `scripts/build_games_disk.py` beside the existing PKWARE DCL port: LHA level-0/1/2 headers, `-lh5-` (LZSS 8 KiB window, Huffman-coded lengths and positions as in the LHa 2.x format), CRC-16 check of every member. References: LHa for UNIX source (`slide.c`, `huf.c`, `header.c`), `lhasa` (`lib/lh5_decoder.c`, `lib/lha_reader.c`), the LHA format description by Osamu Katsura. No external tool at build time.
3. **Games disk**: install `\QUAKE` (and `\QUAKE\ID1`) on `CIUKIGAM.IMG`; grow the disk geometry as needed (18.7 MB more) within the 512 MiB image budget (`scripts/build_image.py` T1 checks, `build-manifest.json`); update `PACKAGES`, the install order, the free-hash guard and the licence records. Memory: Quake needs ≥ 8 MB (16 MB recommended) of DPMI memory, an FPU and a DPMI host (`QUAKE.EXE` is DJGPP: it uses an existing DPMI server or `CWSDPMI.EXE` from its directory); check the dosbox window VM's memory configuration and the DOS system's DPMI host (HDPMI32 in `scripts/test/dos_fixtures.py`?) and make the launch work with 16 MB. Default video mode 13h (320x200), Sound Blaster 16 at `A220 I7 D1 H5 T6` (16-bit auto-init DMA on channel 5: verify `dosvm_sb16.c` supports it), mouse through INT 33h.
4. **Catalogue** (`apps/desktop/catalog.h`): `{"Quake (shareware)", "Quake", "\\QUAKE\\QUAKE.EXE", "input-gaming"}` (characters `autorun.h` can type); icons/tests that count games (`tests/host/desktop`, `test_icons.py`, `test_desktop.py` payload counts, `test_image_payloads.py`).
5. **QEMU case** `quake-qemu-t23` in `tests/suites/f3-workloads.json` mirroring `doom-qemu-t23` (`src/kernel/probes` doom probe: what it waits for; Quake prints its console banner then the game; choose observable markers), host test for the case, and documentation (`docs/design/` games section, `docs/build-and-run.md`).

## Acceptance

`tests/host/test_image_payloads.py`, games-disk tests, desktop host suites green; the lead builds the image, runs `quake-qemu-t23`, then the owner tries Quake on the T23. Report also Quake's documented minimum requirements versus our VM and anything the VM lacks (e.g. VESA for higher modes).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f3-18-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
