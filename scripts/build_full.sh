#!/usr/bin/env bash
set -euo pipefail

mkdir -p build/full

echo "[build-full] generating raw disk scaffold (128MB)"
dd if=/dev/zero of=build/full/ciukios-full.img bs=1M count=128 status=none

cat > build/full/README.txt << 'TXT'
CiukiOS Legacy v2 - Full profile scaffold

Image: ciukios-full.img (128MB)
State: placeholder scaffold (non-bootable in questa fase)
Next: integrare loader legacy + runtime completo
TXT

echo "[build-full] done: build/full/ciukios-full.img"
