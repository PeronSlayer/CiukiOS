# CiukiOS Setup / Installer Project

This directory contains the CiukiOS installer, modelled after classic DOS Setup flows
(multi-floppy or CD-ROM distribution). It is a **separate project** from the runtime
profiles (`floppy/`, `full/`) and targets end-user installation of the full CiukiOS
stack onto a target machine or disk image.

---

## Architecture

```
setup/
  src/
    setup.asm          ; main installer binary (NASM, real-mode)
    setup_ui.asm       ; text-mode TUI (menus, progress bars, prompts)
    setup_disk.asm     ; disk detection, partitioning stubs, FAT write
    setup_copy.asm     ; multi-media file copy engine (floppy swap, CD read)
    setup_cfg.asm      ; configuration writer (SETUP.INF / CIUKIOS.CFG)
  scripts/
    build_setup_floppy.sh  ; produces N × 1.44MB floppy images
    build_setup_cd.sh      ; produces a bootable ISO 9660 image
  media/
    floppy/            ; per-disk staging directories (disk1/, disk2/, ...)
    cd/                ; CD root staging directory
  assets/
    SETUP.INF          ; installation manifest (file list, disk map, defaults)
    WELCOME.TXT        ; splash / license text shown at installer start
```

---

## Distribution Targets

### Multi-Floppy (DOS Setup style)
- Disk 1: boot sector + setup binary + core runtime files
- Disk 2+: payload archives (GEM, apps, optional components)
- Installer prompts "Insert disk N and press ENTER" for each swap
- Final output: N × `ciukios-setup-diskN.img`

### CD-ROM (single bootable ISO)
- El Torito bootable (`ISOLINUX` or custom boot sector)
- Full payload on one disc; no swap prompts
- Final output: `ciukios-setup.iso`

---

## TODO

### Core Installer Binary (`src/setup.asm`)
- [ ] Implement text-mode TUI: welcome screen, license acceptance, partition selector
- [ ] Implement language/keyboard selection menu
- [ ] Implement component selection (Minimal / Standard / Full+GEM)
- [ ] Implement progress bar and per-file copy status

### Disk Engine (`src/setup_disk.asm`)
- [ ] Detect available drives via INT 13h (HDD, FDD, CD)
- [ ] Implement MBR write + partition table creation stub
- [ ] Implement FAT16 format-on-install routine
- [ ] Add CHS/LBA auto-detect for target drive

### Copy Engine (`src/setup_copy.asm`)
- [ ] Implement floppy-swap loop: eject prompt, verify disk label, resume copy
- [ ] Implement CD-ROM read path (INT 13h extended or ATAPI)
- [ ] Implement file-by-file copy from SETUP.INF manifest
- [ ] Add CRC/checksum verification per file

### Configuration (`src/setup_cfg.asm`)
- [ ] Write CIUKIOS.CFG to installed target (timezone, locale, video mode)
- [ ] Write AUTOEXEC.BAT / CONFIG.SYS equivalents

### Floppy Build Script (`scripts/build_setup_floppy.sh`)
- [ ] Define SETUP.INF format and disk-split algorithm
- [ ] Assemble setup.asm → SETUP.COM (must fit in <64KB)
- [ ] Create disk1.img: boot sector + SETUP.COM + Disk1 files
- [ ] Create diskN.img for each subsequent disk
- [ ] Validate each image fits within 1.44MB (2880 sectors)

### CD Build Script (`scripts/build_setup_cd.sh`)
- [ ] Assemble setup binary
- [ ] Stage all payload files under `media/cd/`
- [ ] Generate El Torito boot catalog
- [ ] Pack ISO 9660 image with `genisoimage` or equivalent

### SETUP.INF Manifest
- [ ] Define manifest format (file name, source disk, target path, size, checksum)
- [ ] Populate with full CiukiOS file list (kernel, GEM, apps, fonts, configs)
- [ ] Add component group tags for conditional copy

### Testing
- [ ] QEMU smoke test: boot setup floppy disk1, reach welcome screen, serial marker
- [ ] QEMU smoke test: boot setup CD, reach welcome screen, serial marker
- [ ] Full install simulation: run all disks unattended, verify installed image boots

---

## Dependencies / Prerequisites
- NASM ≥ 3.0
- `mtools` (for floppy image manipulation)
- `genisoimage` or `xorriso` (for CD ISO)
- QEMU (for smoke tests)
- CiukiOS full runtime build must pass before packaging

---

## Notes
- The installer reuses Stage1 DOS runtime services (INT 21h) for file I/O wherever possible.
- The installer binary must not exceed 64KB (single-segment real-mode constraint).
- Target install disk format: FAT16, 128MB minimum recommended.
- Installer UI language: English only for v1; locale stubs planned for v2.
