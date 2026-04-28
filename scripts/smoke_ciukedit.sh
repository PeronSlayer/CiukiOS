#!/usr/bin/env bash
# smoke_ciukedit.sh - Marker-level smoke check for ciukedit.com payload
# Validates: binary exists, size constraint, expected marker strings are present
# Usage: bash scripts/smoke_ciukedit.sh [--floppy|--full]
# Returns 0 on PASS, 1 on any FAIL.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:---floppy}"
case "$PROFILE" in
  --floppy) BUILD_DIR="build/floppy/obj"; IMG="build/floppy/ciukios-floppy.img" ;;
  --full)   BUILD_DIR="build/full/obj";   IMG="build/full/ciukios-full.img"      ;;
  *)
    echo "[smoke-ciukedit] ERROR: unknown profile '$PROFILE'. Use --floppy or --full." >&2
    exit 1
    ;;
esac

BIN="$BUILD_DIR/ciukedit.com"
PASS=0
FAIL=0

check() {
  local label="$1" result="$2"
  if [[ "$result" == "ok" ]]; then
    echo "[smoke-ciukedit] PASS  $label"
    PASS=$((PASS + 1))
  else
    echo "[smoke-ciukedit] FAIL  $label  ($result)"
    FAIL=$((FAIL + 1))
  fi
}

# --- 1. Binary existence ---
if [[ -f "$BIN" ]]; then
  check "ciukedit.com exists ($BUILD_DIR)" "ok"
else
  check "ciukedit.com exists ($BUILD_DIR)" "file not found"
  echo "[smoke-ciukedit] ABORT: binary missing; run make build-${PROFILE#--} first." >&2
  exit 1
fi

# --- 2. Size constraint (must be ≤ 1024 bytes) ---
SIZE="$(stat -c%s "$BIN")"
if [[ "$SIZE" -le 1024 ]]; then
  check "size ≤ 1024 bytes (actual: ${SIZE})" "ok"
else
  check "size ≤ 1024 bytes (actual: ${SIZE})" "exceeded 1024"
fi

# --- 3. Required marker strings present in binary ---
MARKERS=(
  "CiukiDOS EDIT MVP"
  "[CIUKEDIT:BOOT]"
  "[CIUKEDIT:NEW]"
  "[CIUKEDIT:OPEN]"
  "[CIUKEDIT:OK]"
  "[CIUKEDIT:ERR-NOARG]"
  "[CIUKEDIT:ERR-OPEN]"
  "[CIUKEDIT:ERR-WRITE]"
  "CIUKEDIT <filename>"
  "Enter line>"
)

for marker in "${MARKERS[@]}"; do
  if grep -qF "$marker" "$BIN" 2>/dev/null; then
    check "marker present: '$marker'" "ok"
  else
    check "marker present: '$marker'" "not found in binary"
  fi
done

# --- 4. Root directory entry present in disk image ---
if [[ -f "$IMG" ]]; then
  if strings "$IMG" | grep -q "CIUKEDIT"; then
    check "CIUKEDIT entry present in $IMG" "ok"
  else
    check "CIUKEDIT entry present in $IMG" "not found in image (strings)"
  fi
else
  check "disk image exists ($IMG)" "image not found - skip root-entry check"
fi

# --- Summary ---
echo ""
echo "[smoke-ciukedit] Results: ${PASS} PASS / ${FAIL} FAIL (profile: ${PROFILE#--})"
if [[ "$FAIL" -eq 0 ]]; then
  echo "[smoke-ciukedit] STATUS: PASS"
  exit 0
else
  echo "[smoke-ciukedit] STATUS: FAIL"
  exit 1
fi
