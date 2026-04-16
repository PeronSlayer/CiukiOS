#!/usr/bin/env bash
set -euo pipefail

# check_opengem_in_image.sh — Verify OpenGEM files exist in built FAT image
# Usage: ./scripts/check_opengem_in_image.sh [image_path]

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:-$PROJECT_DIR/build/ciukios.img}"

echo "[check-opengem-image] OpenGEM image content probe v1"
echo ""

# Required tools
for tool in mdir; do
    if ! command -v "$tool" &>/dev/null; then
        echo "[FAIL] Required tool not found: $tool (install mtools)"
        exit 1
    fi
done

if [[ ! -f "$IMAGE" ]]; then
    echo "[FAIL] Image not found: $IMAGE"
    echo ""
    echo "Remediation:"
    echo "  CIUKIOS_INCLUDE_OPENGEM=1 ./run_ciukios.sh"
    echo "  # (or just: make)"
    exit 1
fi

FAIL_COUNT=0

# Check 1: FREEDOS/OPENGEM directory exists
echo "[info] checking FREEDOS/OPENGEM directory in image..."
if mdir -i "$IMAGE" ::FREEDOS/OPENGEM/ >/dev/null 2>&1; then
    echo "[OK] ::FREEDOS/OPENGEM/ directory exists"
else
    echo "[FAIL] ::FREEDOS/OPENGEM/ directory NOT FOUND in image"
    echo ""
    echo "Remediation:"
    echo "  1. Run: ./scripts/import_opengem.sh"
    echo "  2. Build: CIUKIOS_INCLUDE_OPENGEM=1 ./run_ciukios.sh"
    ((FAIL_COUNT++)) || true
fi

# Check 2: At least one runnable entry present
echo ""
echo "[info] checking for runnable entry in image..."
ENTRY_FOUND=0
for cand in "GEM.BAT" "GEM.EXE" "OPENGEM.BAT" "OPENGEM.EXE"; do
    if mdir -i "$IMAGE" "::FREEDOS/OPENGEM/$cand" >/dev/null 2>&1; then
        echo "[OK] entry found: ::FREEDOS/OPENGEM/$cand"
        ENTRY_FOUND=1
        break
    fi
done

# Also check nested entry
if [[ "$ENTRY_FOUND" -eq 0 ]]; then
    if mdir -i "$IMAGE" "::FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP" >/dev/null 2>&1; then
        echo "[OK] entry found: ::FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP"
        ENTRY_FOUND=1
    fi
fi

if [[ "$ENTRY_FOUND" -eq 0 ]]; then
    echo "[FAIL] No runnable entry found in ::FREEDOS/OPENGEM/"
    echo ""
    echo "Remediation:"
    echo "  1. Re-run import: ./scripts/import_opengem.sh"
    echo "  2. Rebuild image: CIUKIOS_INCLUDE_OPENGEM=1 ./run_ciukios.sh"
    ((FAIL_COUNT++)) || true
fi

# Check 3: GEMAPPS subdirectory present (sanity check for deep tree)
echo ""
echo "[info] checking GEMAPPS subdirectory..."
if mdir -i "$IMAGE" "::FREEDOS/OPENGEM/GEMAPPS/" >/dev/null 2>&1; then
    echo "[OK] ::FREEDOS/OPENGEM/GEMAPPS/ exists"
else
    echo "[WARN] ::FREEDOS/OPENGEM/GEMAPPS/ not found (may indicate partial import)"
fi

# Summary
echo ""
if [[ "$FAIL_COUNT" -gt 0 ]]; then
    echo "[FAIL] OpenGEM image content probe found $FAIL_COUNT issue(s)"
    exit 1
fi

echo "[PASS] OpenGEM image content probe passed"
