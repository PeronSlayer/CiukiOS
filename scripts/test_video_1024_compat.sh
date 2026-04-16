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

echo "[PASS] 1024x768 compatibility gate passed"
