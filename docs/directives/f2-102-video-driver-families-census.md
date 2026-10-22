# f2-102 — Universal graphics driver families: census, licences and port plan (2D now, 3D input for F7)

- **Phase/model:** F2/F7, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f2-102` from `main`. Research and contract only: **no change under `src/`**.
- **Owner request (2026-10-22 ~23:05):** find a family of universal drivers that gives 2D (and later 3D) acceleration on the common cards of the period (ATI, NVIDIA, Matrox, Intel, …), so that CiukiOS runs accelerated on any retro PC, not only the T23.
- **Interface to target:** `docs/design/display-driver-model.md` (DM1–DM13; the command set DM2 is what every driver must provide).

## Task

1. **Families and devices.**
   - Cover the 1998–2004 baseline (`docs/design/hardware-baseline.md`) plus the qualified laptops (T23 S3 SuperSavage IX/C; E500 ATI Rage Mobility).
   - The families are ATI Mach64/Rage Pro/Rage Mobility, Rage 128 and Radeon R100/R200/R300 (to the X800 of 2004); NVIDIA Riva 128, TNT/TNT2, GeForce 256/2/3/4/FX/6; Matrox G200/G400/G450/G550; Intel i740, i810/i815 and i830/i845G/i855GM/i865G/i915G; 3dfx Voodoo3/4/5; S3 Trio64/ViRGE/Savage/ProSavage/SuperSavage; VIA UniChrome; SiS 300/315/330; Trident Blade/CyberBlade; Cirrus Logic GD54xx; NeoMagic; Chips & Technologies.
   - Add the virtual adapters useful for tests: QEMU std VGA, cirrus-vga, `ati-vga` (Rage 128 Pro and Radeon M6 models), vmware-svga, bochs-display, and VirtualBox VMSVGA.
   - Count which of them the owner's driver pack lists as Display class (from `assets/hwids/pci-ids.json` and the pack's INFs) to weigh how common each family is.
2. **Candidate sources per family, with exact upstream paths and commit or tag.**
   - **Haiku accelerants and their kernel drivers:** `src/add-ons/accelerants/*`, `src/add-ons/kernel/drivers/graphics/*`.
   - **Linux fbdev drivers:** `drivers/video/fbdev/*`, for example atyfb, aty128fb, radeonfb, rivafb, nvidiafb, matroxfb, i810fb, intelfb, tdfxfb, s3fb, savagefb, sisfb, tridentfb, neofb, cirrusfb, viafb. Note any that recent kernels have removed, with the tag where each still exists.
   - **X.org drivers:** xf86-video-ati/r128/mach64, nv, mga, i810/intel (old), tdfx, s3, s3virge, savage, sis, trident, neomagic, cirrus, chips, openchrome, vmware.
   - **Licences:** read the licence header of **every file** a port would take. Haiku is not assumed to be MIT file by file. The kernel is GPL-2.0-only, so GPL-2.0-or-later, GPL-2.0, MIT and BSD are compatible; GPL-3.0-only and unclear terms are not. Report every exception.
3. **Capability map per source:**
   - the DM2 commands it implements (fill, copy, mono expansion, write, invert, keyed, scaled, lines, mono and colour cursor);
   - native mode setting, or whether VBE is needed;
   - overlay or video scaler;
   - known hardware bugs or workarounds (cite lines);
   - the host API it needs (Haiku: `create_area`, `get_pci_info`, benaphores, `snooze`; Linux: `fb_info`, `ioremap`, PCI helpers);
   - an estimated shim and port size in lines of code.
4. **3D input for F7, research only.**
   - Mesa "Amber" 21.3 classic drivers (r100, r200, i915, nouveau_vieux) and Mesa 7.11 DRI1 drivers (mga, r128, savage, tdfx, sis, unichrome, i810).
   - Gallium r300, nv30, i915g and crocus.
   - The kernel DRM or DRI1 services each needs.
   - Each entry gets its licence and a size estimate, and the result is compared with `docs/design/f7-3d-acceleration-contract.md`.
5. **Contract** `docs/design/video-driver-families.md`:
   - one table per family with the chosen source and its reason, its PCI ids, licence, commands, mode setting, test path (QEMU device or hardware only), size and risks;
   - the shim layer decision: one Haiku-accelerant host, one Linux-fbdev host, or per-driver native ports, with the trade-off;
   - the ordered directive plan f2-103… by family, most common first;
   - an honest expectation of what can be verified without the hardware.

## Acceptance

The contract document is complete, every source carries a link and a licence verdict, and nothing under `src/` changes. A census script is committed only if the build uses it; otherwise the tables go into the document.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Downloaded sources go under the worktree's `build/research/` (ignored) and are never committed. Write `build/codex/f2-102-report.md`.
