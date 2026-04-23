#!/usr/bin/env bash
# build_setup_floppy.sh — Build CiukiOS multi-floppy setup images
# TODO:
#   1. Assemble setup/src/setup.asm → build/setup/SETUP.COM
#   2. Read setup/media/floppy/SETUP.INF to determine disk split
#   3. For each disk N:
#      a. Create blank 1.44MB FAT12 image (2880 sectors)
#      b. Inject boot sector (from floppy_boot.asm or a setup-specific one)
#      c. Copy SETUP.COM (disk 1 only)
#      d. Copy files assigned to disk N from SETUP.INF
#      e. Validate image size ≤ 1.44MB
#      f. Output: build/setup/ciukios-setup-disk{N}.img
#   4. Print summary: N disks, total payload size

set -euo pipefail
echo "[build-setup-floppy] NOT YET IMPLEMENTED"
exit 1
