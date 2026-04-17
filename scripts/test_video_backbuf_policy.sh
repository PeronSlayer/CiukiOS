#!/usr/bin/env bash
# Non-interactive static analysis test for video backbuffer dynamic policy.
# Validates that the backbuffer budget, allocation, and rendering path
# are consistent across video_limits.h, video.c, and loader.c.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIMITS_H="$PROJECT_DIR/boot/proto/video_limits.h"
VIDEO_C="$PROJECT_DIR/stage2/src/video.c"
LOADER_C="$PROJECT_DIR/boot/uefi-loader/loader.c"

fail=0

for f in "$LIMITS_H" "$VIDEO_C" "$LOADER_C"; do
    if [[ ! -f "$f" ]]; then
        echo "[FAIL] required file not found: $f" >&2
        exit 1
    fi
done

# --- Parse limits ---
max_w=$(grep -E "^#define[[:space:]]+VIDEO_DRIVER_MAX_W" "$LIMITS_H" | sed -E 's/.*[[:space:]]([0-9]+)U.*/\1/' | head -n1)
max_h=$(grep -E "^#define[[:space:]]+VIDEO_DRIVER_MAX_H" "$LIMITS_H" | sed -E 's/.*[[:space:]]([0-9]+)U.*/\1/' | head -n1)
max_bpp=$(grep -E "^#define[[:space:]]+VIDEO_DRIVER_MAX_BPP" "$LIMITS_H" | sed -E 's/.*[[:space:]]([0-9]+)U.*/\1/' | head -n1)

if [[ -z "$max_w" || -z "$max_h" || -z "$max_bpp" ]]; then
    echo "[FAIL] cannot parse VIDEO_DRIVER_MAX_* from $LIMITS_H" >&2
    exit 1
fi

budget=$(( max_w * max_h * max_bpp ))
echo "[INFO] backbuffer budget: ${max_w}x${max_h}x${max_bpp} = ${budget} bytes ($(( budget / 1024 / 1024 )) MB)"

# --- Check 1: Budget >= 1024x768 (backward compat) ---
min_budget=$(( 1024 * 768 * 4 ))
if (( budget < min_budget )); then
    echo "[FAIL] budget ${budget} < minimum ${min_budget} (1024x768x4)" >&2
    fail=1
else
    echo "[OK] budget covers 1024x768 baseline"
fi

# --- Check 2: Budget >= 1920x1080 (extended target) ---
fhd_budget=$(( 1920 * 1080 * 4 ))
if (( budget < fhd_budget )); then
    echo "[WARN] budget ${budget} < 1920x1080 target ${fhd_budget}"
else
    echo "[OK] budget covers 1920x1080 (Full HD)"
fi

# --- Check 3: video.c uses VIDEO_BACKBUF_MAX_BYTES for static allocation ---
if grep -q 'g_backbuf\[VIDEO_BACKBUF_MAX_BYTES\]' "$VIDEO_C"; then
    echo "[OK] video.c allocates backbuffer via VIDEO_BACKBUF_MAX_BYTES"
else
    echo "[FAIL] video.c does not use VIDEO_BACKBUF_MAX_BYTES for allocation" >&2
    fail=1
fi

# --- Check 4: video.c has dynamic double-buffer decision ---
if grep -q 'needed <= VIDEO_BACKBUF_MAX_BYTES' "$VIDEO_C"; then
    echo "[OK] video.c has dynamic backbuffer decision (needed vs budget)"
else
    echo "[FAIL] video.c missing dynamic backbuffer size check" >&2
    fail=1
fi

# --- Check 5: video.c emits backbuf diagnostic ---
if grep -q 'backbuf_budget=' "$VIDEO_C"; then
    echo "[OK] video.c emits backbuffer budget diagnostic"
else
    echo "[FAIL] video.c missing backbuffer budget diagnostic" >&2
    fail=1
fi

# --- Check 6: loader.c emits backbuf diagnostic ---
if grep -q 'backbuf budget=' "$LOADER_C"; then
    echo "[OK] loader.c emits backbuffer budget diagnostic"
else
    echo "[FAIL] loader.c missing backbuffer budget diagnostic" >&2
    fail=1
fi

# --- Check 7: loader.c catalog populates flags with fits_backbuf ---
if grep -q 'entry->flags = fits_backbuf' "$LOADER_C"; then
    echo "[OK] loader.c sets catalog flags from fits_backbuf"
else
    echo "[FAIL] loader.c catalog flags not derived from fits_backbuf" >&2
    fail=1
fi

# --- Check 8: fallback preference table still gated on fits_backbuf ---
if grep -B1 'fallback preference table' "$LOADER_C" | grep -q 'fits_backbuf' ||
   grep -A1 'fallback preference table' "$LOADER_C" | grep -q 'fits_backbuf'; then
    echo "[OK] fallback preference table requires fits_backbuf (deterministic)"
else
    echo "[FAIL] fallback preference table not gated on fits_backbuf" >&2
    fail=1
fi

# --- Check 9: VIDEO_POLICY_BASELINE constants ---
bl_w=$(grep -E "^#define[[:space:]]+VIDEO_POLICY_BASELINE_W" "$LIMITS_H" | sed -E 's/.*[[:space:]]([0-9]+)U.*/\1/' | head -n1)
bl_h=$(grep -E "^#define[[:space:]]+VIDEO_POLICY_BASELINE_H" "$LIMITS_H" | sed -E 's/.*[[:space:]]([0-9]+)U.*/\1/' | head -n1)

if [[ "$bl_w" == "1024" && "$bl_h" == "768" ]]; then
    echo "[OK] baseline policy is 1024x768"
else
    echo "[FAIL] baseline policy is not 1024x768 (found ${bl_w:-?}x${bl_h:-?})" >&2
    fail=1
fi

if (( fail )); then
    echo "[FAIL] backbuffer policy test failed"
    exit 1
fi

echo "[PASS] backbuffer policy test completed"
