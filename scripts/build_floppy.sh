#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/floppy

echo "[build-floppy] generating 1.44MB image scaffold"
dd if=/dev/zero of=build/floppy/ciukios-floppy.img bs=512 count=2880 status=none

cat > build/floppy/README.txt << 'TXT'
CiukiOS Legacy v2 - Floppy profile scaffold

Image: ciukios-floppy.img (1.44MB)
State: placeholder scaffold (not fully bootable yet)
Next: implement legacy BIOS boot sector + stage loader
TXT

echo "[build-floppy] done: build/floppy/ciukios-floppy.img"
