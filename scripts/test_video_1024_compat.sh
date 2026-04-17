#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIMITS_H="$PROJECT_DIR/boot/proto/video_limits.h"
LOADER_C="$PROJECT_DIR/boot/uefi-loader/loader.c"

if [[ ! -f "$LIMITS_H" || ! -f "$LOADER_C" ]]; then
    echo "[FAIL] required files not found" >&2
    exit 1
fi

max_w=$(grep -E "^#define[[:space:]]+VIDEO_DRIVER_MAX_W" "$LIMITS_H" | sed -E 's/.*VIDEO_DRIVER_MAX_W[[:space:]]+([0-9]+)U.*/\1/' | head -n1)
max_h=$(grep -E "^#define[[:space:]]+VIDEO_DRIVER_MAX_H" "$LIMITS_H" | sed -E 's/.*VIDEO_DRIVER_MAX_H[[:space:]]+([0-9]+)U.*/\1/' | head -n1)

if [[ -z "$max_w" || -z "$max_h" ]]; then
    echo "[FAIL] unable to parse VIDEO_DRIVER_MAX_* limits" >&2
    exit 1
fi

if (( max_w < 1024 || max_h < 768 )); then
    echo "[FAIL] driver limits below 1024x768 (found ${max_w}x${max_h})" >&2
    exit 1
fi
echo "[OK] video driver limits support >= 1024x768 (${max_w}x${max_h})"

# Verify baseline policy constants exist
baseline_w=$(grep -E "^#define[[:space:]]+VIDEO_POLICY_BASELINE_W" "$LIMITS_H" | sed -E 's/.*VIDEO_POLICY_BASELINE_W[[:space:]]+([0-9]+)U.*/\1/' | head -n1)
baseline_h=$(grep -E "^#define[[:space:]]+VIDEO_POLICY_BASELINE_H" "$LIMITS_H" | sed -E 's/.*VIDEO_POLICY_BASELINE_H[[:space:]]+([0-9]+)U.*/\1/' | head -n1)

if [[ -z "$baseline_w" || -z "$baseline_h" ]]; then
    echo "[FAIL] missing VIDEO_POLICY_BASELINE_W/H constants" >&2
    exit 1
fi
if (( baseline_w != 1024 || baseline_h != 768 )); then
    echo "[FAIL] baseline policy must be 1024x768 (found ${baseline_w}x${baseline_h})" >&2
    exit 1
fi
echo "[OK] baseline policy constants correct (${baseline_w}x${baseline_h})"

# Verify backbuffer budget covers at least 1920x1080
backbuf_bytes=$(( max_w * max_h * 4 ))
fullhd_bytes=$(( 1920 * 1080 * 4 ))
if (( backbuf_bytes < fullhd_bytes )); then
    echo "[WARN] backbuffer budget ${backbuf_bytes} does not cover 1920x1080 (${fullhd_bytes})"
else
    echo "[OK] backbuffer budget covers 1920x1080 (budget=${backbuf_bytes} needed=${fullhd_bytes})"
fi

if ! grep -Fq "{1024, 768}" "$LOADER_C"; then
    echo "[FAIL] loader preferred GOP list missing 1024x768" >&2
    exit 1
fi
echo "[OK] loader preferred GOP list contains 1024x768"

line_1024=$(grep -nF "{1024, 768}" "$LOADER_C" | head -n1 | cut -d: -f1)
line_800=$(grep -nF "{800,  600}" "$LOADER_C" | head -n1 | cut -d: -f1)

if [[ -n "$line_800" && -n "$line_1024" ]]; then
    if (( line_1024 > line_800 )); then
        echo "[FAIL] 1024x768 is ranked after 800x600 in preferred list" >&2
        exit 1
    fi
    echo "[OK] 1024x768 priority is >= 800x600"
fi

# Verify higher-res modes exist in preferred table
for mode in "1280, 720" "1920, 1080"; do
    if grep -Fq "{${mode}}" "$LOADER_C"; then
        echo "[OK] preferred table includes ${mode// /}"
    fi
done

# Verify VMODE.CFG path does not gate on fits_backbuf
if grep -A2 'VMODE.CFG priority check' "$LOADER_C" | grep -q 'fits_backbuf'; then
    echo "[FAIL] VMODE.CFG selection still gated on fits_backbuf" >&2
    exit 1
fi
echo "[OK] VMODE.CFG selection accepts any 32bpp mode"

echo "[PASS] 1024x768 compatibility gate passed"
