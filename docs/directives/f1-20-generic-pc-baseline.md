# Directive f1-20: generic PC baseline — assembled desktops of the era

- **Step:** F1 (hardware scope). **Owner decision 2026-10-11:** besides the
  two qualified laptops, CiukiOS targets generic assembled desktop PCs with
  the common configurations of the era (roughly 1998–2004). One image for
  every machine; detection at boot; no per-machine builds.
- **Contracts:** `device-firmware-ownership.md` (BIOS boundary, PCI
  configuration mechanism #1, PIC/PIT ownership), `boot-memory.md` (E820,
  VBE), `f0-acceptance.md`/`f1-acceptance.md` (emulation profiles,
  evidence), `foundations-transition.md` (scope).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`, web search on.
- **Worktree:** `wt/f1-generic-pc`. Files: new `docs/design/hardware-baseline.md`,
  `docs/README.md` (index line), new `tests/profiles/qemu-desktop-1998.json`
  and `tests/profiles/qemu-desktop-2002.json`, `tests/suites/f0-core.json`,
  `f1-input.json`, `f1-safe.json` (add the two profiles to the boot matrix
  and the probe cases that depend on the display/input path),
  `scripts/test/run.py` only if a profile field is missing,
  `src/boot/ciukldr/platform.inc` and `memory.inc` (CPU and firmware
  minimum checks with a clear screen/serial message), `scripts/test/loader_model.py`
  and `tests/host/test_loader_model.py`, `tests/host/test_runner.py`.

## What to write and build

1. **`hardware-baseline.md`** (research with primary sources: Intel/AMD
   CPU feature docs, VESA VBE 2.0/3.0, i440BX/i815/VIA Apollo/nForce2
   datasheets or vendor docs, PCI 2.2, ATA/ATAPI-6, PS/2 keyboard/mouse,
   SB16/AC97 only as pointers for F4): the supported baseline a generic PC
   must meet, with the reason for each line:
   - CPU: i686 class with CPUID and CMOV (Pentium Pro/II/III/4, Celeron,
     Athlon/Duron/XP, VIA C3 Nehemiah); FXSR optional; no SSE required;
     explicitly **not** supported: Pentium/MMX and AMD K6 family (no CMOV;
     the kernel and SDK are built with `-march=pentiumpro`). State the owner's rule
     of 2026-10-11: the main image stays i686; at most one additional
     build and image for the oldest baseline (i586, no CMOV) may be made
     later if requested, never per-machine images.
   - Firmware: BIOS with E820 (1997+), VBE 2.0+ with a linear framebuffer
     mode at 640×480 or better, A20 via keyboard controller or port 92h,
     INT 13h extensions for the disk; PCI configuration mechanism #1.
   - Memory: 64 MiB minimum for F0–F2 (the 128 MiB profile is the
     reference; state what 64 MiB would need), up to 3 GiB usable.
   - Storage: parallel ATA PIO (any chipset with legacy ports 1F0/170),
     MBR + FAT32 boot volume; ATAPI CD for install later (F4); SATA in
     legacy IDE mode accepted, AHCI not.
   - Input: PS/2 keyboard and mouse (native i8042); USB keyboards/mice
     through the BIOS USB-legacy emulation count as PS/2 while it is
     active; explain the limit (no USB stack before a later step).
   - Display: any VBE 2.0+ card with an LFB (S3, ATI Rage, nVidia TNT/
     GeForce, Matrox, Cirrus, Intel 8xx, Voodoo3): framebuffer only in F1;
     acceleration later and per chip.
   - Audio and network: out of scope here (F4/F6), list the common chips
     to plan for (SB16/SB Live!, ES1370/1371, AC'97 ICH; RTL8139, 3C905,
     NE2000, Intel PRO/100).
   - A **compatibility matrix table** with columns machine class / CPU /
     chipset / status (qualified on hardware, emulated on QEMU, expected,
     unsupported) initialised with: ThinkPad T23, Armada E500, "1998
     desktop" (Pentium II, i440BX, Cirrus/S3, PS/2, ATA), "2002 desktop"
     (Athlon XP or Pentium 4, VIA KT266/i845, GeForce, PS/2, ATA), and the
     QEMU twins; the owner's own assembled PCs are added when he tests.
   - Policy for unknown hardware: generic paths first (VBE LFB, i8042,
     ATA PIO, PIC/PIT), no vendor driver activation without qualification,
     safe mode always available, clear refusal messages for unmet minimums.
2. **QEMU profiles**: `qemu-desktop-1998` (`pc-i440fx`, `-cpu pentium2`,
   128 MiB, `-vga cirrus`, PS/2, IDE, TCG with icount) and
   `qemu-desktop-2002` (`pc-i440fx`, `-cpu athlon` if QEMU accepts it, else
   `pentium3`, 512 MiB, `-vga std`, PS/2, IDE); document SeaVGABIOS's VBE
   behaviour for cirrus vs std and record the deviations as for the T23.
3. **Loader minimum checks**: CPUID presence and CMOV (and the family/
   model/stepping recorded in the boot info diagnostics), E820 presence,
   VBE 2.0+; failures print one line on screen and serial (`CIUKI: this PC
   needs an i686 CPU with CMOV (Pentium Pro or later)`) and halt without
   touching the disk; mirror in `loader_model.py` with tests.
4. **Suites**: the boot matrix of `f0-core` and the `f1-input`/`f1-safe`
   cases gain the two desktop profiles (keep the runtime bounded: ten
   boots on each new profile, probes once per profile).

## Acceptance by the lead

Host tests; loader assembly; on QEMU `f0-smoke` and the new boots of
`f0-core` pass on both desktop profiles; `f1-safe` and `f1-input` (native)
pass on `qemu-desktop-1998` (Cirrus LFB) and `qemu-desktop-2002`. Reply
with: the baseline table, the profile definitions, test output, and any
contract problem found (for example VBE modes SeaVGABIOS offers on
cirrus).
