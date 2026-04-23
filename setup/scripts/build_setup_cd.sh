#!/usr/bin/env bash
# build_setup_cd.sh — Build CiukiOS bootable CD-ROM setup ISO
# TODO:
#   1. Assemble setup/src/setup.asm → build/setup/SETUP.COM
#   2. Stage all payload files under build/setup/cd-root/
#   3. Copy SETUP.INF, WELCOME.TXT, runtime binaries to cd-root/
#   4. Generate El Torito boot catalog (boot sector from floppy_boot.asm)
#   5. Pack ISO 9660 with genisoimage or xorriso:
#        genisoimage -b BOOT.IMG -c BOOT.CAT -no-emul-boot \
#          -boot-load-size 4 -boot-info-table -o ciukios-setup.iso cd-root/
#   6. Output: build/setup/ciukios-setup.iso

set -euo pipefail
echo "[build-setup-cd] NOT YET IMPLEMENTED"
exit 1
