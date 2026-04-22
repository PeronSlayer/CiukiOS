#!/usr/bin/env bash
set -euo pipefail

# Video Policy Matrix Gate (P1-V3)
# Validates policyv2/budgetv2 markers in source and build artifacts.
# Usage: ./scripts/test_video_policy_matrix.sh

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOADER_C="$PROJECT_DIR/boot/uefi-loader/loader.c"
LIMITS_H="$PROJECT_DIR/boot/proto/video_limits.h"
VIDEO_C="$PROJECT_DIR/stage2/src/video.c"
UI_C="$PROJECT_DIR/stage2/src/ui.c"

pass=0
fail=0
total=0

gate() {
    local desc="$1"
    local result="$2"
    total=$((total + 1))
    if [ "$result" -eq 0 ]; then
        echo "[PASS] $desc"
        pass=$((pass + 1))
    else
        echo "[FAIL] $desc"
        fail=$((fail + 1))
    fi
}

echo "=== Video Policy Matrix Gate (P1-V3) ==="
echo ""

# -------------------------------------------------------------------
# G1: policyv2 scoring engine present in loader
# -------------------------------------------------------------------
if grep -Fq 'policyv2 modes=' "$LOADER_C" && grep -Fq 'score=' "$LOADER_C"; then
    gate "P1-V1: policyv2 scoring marker in loader.c" 0
else
    gate "P1-V1: policyv2 scoring marker in loader.c" 1
fi

# G2: resolution class table covers mandated resolutions
missing_res=0
for res in "1024,  768" "1280,  720" "1280,  800" "1600,  900" "1920, 1080" "2560, 1440" "3840, 2160"; do
    if ! grep -q "${res}" "$LOADER_C"; then
        echo "  [INFO] resolution class not found: ${res}"
        missing_res=1
    fi
done
gate "P1-V1: resolution class table covers mandated set" "$missing_res"

# G3: PASS/FALLBACK result marker
if grep -q 'result_str.*=.*L"PASS"' "$LOADER_C" && grep -q 'result_str.*=.*L"FALLBACK"' "$LOADER_C"; then
    gate "P1-V1: PASS/FALLBACK result paths in loader.c" 0
else
    gate "P1-V1: PASS/FALLBACK result paths in loader.c" 1
fi

# G4: budgetv2 markers in loader
if grep -Fq 'budgetv2 class=' "$LOADER_C" && grep -Fq 'allow_db=' "$LOADER_C"; then
    gate "P1-V2: budgetv2 marker in loader.c" 0
else
    gate "P1-V2: budgetv2 marker in loader.c" 1
fi

# G5: budget tier defines in video_limits.h
tier_ok=0
for tier in VIDEO_BUDGET_TIER_BASELINE_BYTES VIDEO_BUDGET_TIER_HD_BYTES VIDEO_BUDGET_TIER_HDP_BYTES VIDEO_BUDGET_TIER_FHD_BYTES VIDEO_BUDGET_TIER_QHD_BYTES VIDEO_BUDGET_TIER_4K_BYTES; do
    if ! grep -Fq "$tier" "$LIMITS_H"; then
        echo "  [INFO] missing budget tier: $tier"
        tier_ok=1
    fi
done
gate "P1-V2: budget tier defines in video_limits.h" "$tier_ok"

# G6: safe ceiling define
if grep -Fq 'VIDEO_BUDGET_SAFE_CEILING' "$LIMITS_H"; then
    gate "P1-V2: safe ceiling define in video_limits.h" 0
else
    gate "P1-V2: safe ceiling define in video_limits.h" 1
fi

# G7: budgetv2 marker in video.c (stage2 side)
if grep -Fq 'budgetv2 class=' "$VIDEO_C"; then
    gate "P1-V2: budgetv2 marker in video.c" 0
else
    gate "P1-V2: budgetv2 marker in video.c" 1
fi

# G8: fallback reason marker in loader
if grep -Fq 'fallback reason=' "$LOADER_C"; then
    gate "P1-V2: fallback reason marker in loader.c" 0
else
    gate "P1-V2: fallback reason marker in loader.c" 1
fi

# G9: preferred table still contains 1024x768 (backward compat)
if grep -Fq '{1024, 768}' "$LOADER_C"; then
    gate "backward compat: preferred table has 1024x768" 0
else
    gate "backward compat: preferred table has 1024x768" 1
fi

# G10: no panic/UD references in scoring path
if grep -q 'panic\|#UD\|Invalid Opcode' "$LOADER_C" 2>/dev/null; then
    gate "safety: no panic/#UD markers in loader scoring" 1
else
    gate "safety: no panic/#UD markers in loader scoring" 0
fi

# G11: ui layout matrix markers present
if grep -Fq 'layout matrix pass' "$UI_C"; then
    gate "P1-V4: layout matrix markers in ui.c" 0
else
    gate "P1-V4: layout matrix markers in ui.c" 1
fi

# G12: build succeeds
if make -C "$PROJECT_DIR" all >/dev/null 2>&1; then
    gate "build: make all succeeds" 0
else
    gate "build: make all succeeds" 1
fi

echo ""
echo "=== Policy Matrix Gate: ${pass}/${total} passed ==="

if [ "$fail" -gt 0 ]; then
    echo "[FAIL] Policy matrix gate: $fail failure(s)"
    exit 1
fi

echo "[PASS] Policy matrix gate passed"
