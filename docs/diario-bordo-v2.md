# Engineering Logbook - CiukiOS Legacy v2

## 2026-04-22
1. Decision: full architectural reset toward legacy BIOS x86.
2. Action: previous project archived to `OLD/archive-2026-04-22/`.
3. Action: new documentation baseline created (architecture, roadmap, AI directives).
4. Action: versioning baseline reset to `pre-Alpha v0.5.0`.
5. Operating constraint: branch-only development, merge/push only after explicit user confirmation.
6. Milestone: delivered a BIOS-bootable `floppy` stage0 baseline with a real 16-bit boot sector.
7. Milestone: upgraded floppy QEMU smoke testing to assert a concrete boot marker.
8. Next step: add stage1 loader and disk-read path beyond the boot sector.
