#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/floppy

echo "[build-floppy] generating 1.44MB image scaffold"
dd if=/dev/zero of=build/floppy/ciukios-floppy.img bs=512 count=2880 status=none

cat > build/floppy/README.txt << 'TXT'
CiukiOS Legacy v2 - Floppy profile scaffold

Image: ciukios-floppy.img (1.44MB)
State: placeholder scaffold (non-bootable in questa fase)
Next: implementare boot sector + stage loader legacy BIOS
TXT

echo "[build-floppy] done: build/floppy/ciukios-floppy.img"
