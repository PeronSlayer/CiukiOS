CiukiOS integration notes for ctmouse

Source:
- Fork: https://github.com/PeronSlayer/ctmouse
- Imported snapshot: local vendor copy under third_party/ctmouse

Runtime integration:
- build_full.sh injects CTMOUSE.EXE into the full FAT16 image root.
- stage2 (full_stage2.asm) tries to run CTMOUSE.EXE before GEMVDI.
- Launch is best-effort: boot continues if CTMOUSE is missing or fails.

License:
- See third_party/ctmouse/copying
- Keep upstream license and notices intact when updating this vendor copy.
