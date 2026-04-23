#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LABEL="${1:-latest}"
OUT_DIR="build/full/opengem-hardware-lane-${LABEL}"
mkdir -p "$OUT_DIR"

cp -f docs/templates/opengem-hardware-execution-template.md "$OUT_DIR/execution-template.md"
cp -f docs/templates/opengem-hardware-evidence-template.json "$OUT_DIR/evidence-template.json"
cp -f docs/opengem-hardware-validation-lane.md "$OUT_DIR/hardware-lane-guide.md"

cat > "$OUT_DIR/README.txt" << 'TXT'
OpenGEM hardware lane package

1. Fill execution-template.md during manual hardware run.
2. Copy/rename evidence-template.json and populate measured values.
3. Attach BIOS photos, serial captures, and notes alongside these files.
TXT

echo "[opengem-hw-pack] created: $OUT_DIR"
