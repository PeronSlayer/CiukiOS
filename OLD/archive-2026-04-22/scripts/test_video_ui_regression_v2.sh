#!/usr/bin/env bash
set -euo pipefail

# Video/UI Regression Gate v2
# Validates presence of new V1-V4 markers and absence of regressions.
# Usage: ./scripts/test_video_ui_regression_v2.sh

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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

echo "=== Video/UI Regression Gate v2 ==="
echo ""

# -------------------------------------------------------------------
# Static gates: source code validation (no QEMU needed)
# -------------------------------------------------------------------

# G1: V1 overlay APIs exist in video.h
if grep -q 'video_overlay_mark_dirty' "$PROJECT_DIR/stage2/include/video.h" &&
   grep -q 'video_overlay_present_dirty' "$PROJECT_DIR/stage2/include/video.h" &&
   grep -q 'video_overlay_clear_region' "$PROJECT_DIR/stage2/include/video.h"; then
    gate "V1: overlay APIs declared in video.h" 0
else
    gate "V1: overlay APIs declared in video.h" 1
fi

# G2: V1 overlay marker in video.c
if grep -Fq '[video] overlay plane active' "$PROJECT_DIR/stage2/src/video.c"; then
    gate "V1: overlay plane marker in video.c" 0
else
    gate "V1: overlay plane marker in video.c" 1
fi

# G3: V2 pacing APIs exist in video.h
if grep -q 'video_pacing_init' "$PROJECT_DIR/stage2/include/video.h" &&
   grep -q 'video_pacing_should_present' "$PROJECT_DIR/stage2/include/video.h" &&
   grep -q 'video_pacing_report' "$PROJECT_DIR/stage2/include/video.h"; then
    gate "V2: pacing APIs declared in video.h" 0
else
    gate "V2: pacing APIs declared in video.h" 1
fi

# G4: V2 pacing marker in video.c
if grep -Fq '[video] pacing stable present_full=' "$PROJECT_DIR/stage2/src/video.c"; then
    gate "V2: pacing report marker in video.c" 0
else
    gate "V2: pacing report marker in video.c" 1
fi

# G5: V2 present counters track in video_present / video_present_dirty
if grep -q 'g_present_full_count' "$PROJECT_DIR/stage2/src/video.c" &&
   grep -q 'g_present_dirty_count' "$PROJECT_DIR/stage2/src/video.c"; then
    gate "V2: present counters exist in video.c" 0
else
    gate "V2: present counters exist in video.c" 1
fi

# G6: V3 layout metrics API in ui.h
if grep -q 'ui_metrics_t' "$PROJECT_DIR/stage2/include/ui.h" &&
   grep -q 'ui_metrics_apply' "$PROJECT_DIR/stage2/include/ui.h"; then
    gate "V3: layout metrics API in ui.h" 0
else
    gate "V3: layout metrics API in ui.h" 1
fi

# G7: V3 layout metrics marker in ui.c
if grep -Fq '[ui] layout metrics v3 active' "$PROJECT_DIR/stage2/src/ui.c"; then
    gate "V3: layout metrics marker in ui.c" 0
else
    gate "V3: layout metrics marker in ui.c" 1
fi

# G8: V3 clipping guards in layout
if grep -q 'Clipping guard' "$PROJECT_DIR/stage2/src/ui.c"; then
    gate "V3: clipping guards in ui_compute_layout" 0
else
    gate "V3: clipping guards in ui_compute_layout" 1
fi

# G9: V4 font profile APIs in video.h
if grep -q 'video_select_font_profile' "$PROJECT_DIR/stage2/include/video.h" &&
   grep -q 'video_get_font_profile_name' "$PROJECT_DIR/stage2/include/video.h"; then
    gate "V4: font profile APIs in video.h" 0
else
    gate "V4: font profile APIs in video.h" 1
fi

# G10: V4 font profile marker in video.c
if grep -Fq '[video] font profile=' "$PROJECT_DIR/stage2/src/video.c"; then
    gate "V4: font profile marker in video.c" 0
else
    gate "V4: font profile marker in video.c" 1
fi

# G11: V4 two font scales (small/normal)
if grep -q 'FONT_PROFILE_SMALL' "$PROJECT_DIR/stage2/src/video.c" &&
   grep -q 'FONT_PROFILE_NORMAL' "$PROJECT_DIR/stage2/src/video.c"; then
    gate "V4: small/normal font profiles defined" 0
else
    gate "V4: small/normal font profiles defined" 1
fi

# -------------------------------------------------------------------
# Negative regression checks
# -------------------------------------------------------------------

# G12: No ABI marker removal — existing markers must remain
missing_markers=0
for marker in \
    "[video] mode=" \
    "[video] backbuf_budget=" \
    "[ ui ] desktop layout v2 active" \
    "[ ui ] window chrome v2 ready" \
    "[ ui ] desktop shell surface active" \
    "[ ui ] alignment surgical v6 active" \
    "[ ui ] desktop focus ux v8 active" \
    "[ ui ] desktop layout manager v3 active"; do
    found=0
    if grep -rFq "$marker" "$PROJECT_DIR/stage2/src/video.c" 2>/dev/null; then found=1; fi
    if grep -rFq "$marker" "$PROJECT_DIR/stage2/src/ui.c"    2>/dev/null; then found=1; fi
    if grep -rFq "$marker" "$PROJECT_DIR/stage2/src/shell.c" 2>/dev/null; then found=1; fi
    if [ "$found" -eq 0 ]; then
        echo "  [!] missing existing marker: $marker"
        missing_markers=1
    fi
done
gate "Existing serial markers preserved" $missing_markers

# G13: Build succeeds
if make -C "$PROJECT_DIR" all >/dev/null 2>&1; then
    gate "make all compiles cleanly" 0
else
    gate "make all compiles cleanly" 1
fi

# G14: No panic/fault keywords in new code
new_fault=0
for kw in "panic" "#UD" "General Protection Fault"; do
    if grep -Fq "$kw" "$PROJECT_DIR/stage2/src/video.c" 2>/dev/null; then
        echo "  [!] fault keyword '$kw' found in video.c"
        new_fault=1
    fi
done
gate "No panic/fault keywords in video.c" $new_fault

echo ""
echo "=== SUMMARY: PASS=$pass / FAIL=$fail / TOTAL=$total ==="

if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
