#!/usr/bin/env bash
set -euo pipefail

# OpenGEM Integration Smoke Test v2
# Validates OpenGEM integration markers, command surface, and preflight wiring
# Usage: ./scripts/test_opengem_integration.sh [logfile]
#
# Returns: PASS (with optional SKIP semantics if payload absent)

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${1:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"
OPENGEM_RUNTIME="$PROJECT_DIR/third_party/freedos/runtime/OPENGEM"
SHELL_FILE="$PROJECT_DIR/stage2/src/shell.c"
RUN_FILE="$PROJECT_DIR/run_ciukios.sh"
IMAGE_PROBE_FILE="$PROJECT_DIR/scripts/check_opengem_in_image.sh"

echo "[test-opengem] OpenGEM integration smoke test v2"
echo ""

FAIL_COUNT=0

check_static() {
    local pattern="$1"
    local file="$2"
    local desc="$3"
    if grep -Fq "$pattern" "$file" 2>/dev/null; then
        echo "[OK] $desc"
    else
        echo "[FAIL] $desc"
        ((FAIL_COUNT++)) || true
    fi
}

check_marker() {
    local marker="$1"
    local severity="$2"  # FAIL, WARN, INFO, SKIP
    if [[ -f "$LOG_FILE" ]] && grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
        return 0
    else
        echo "[$severity] absent: $marker"
        if [[ "$severity" == "FAIL" ]]; then
            ((FAIL_COUNT++)) || true
        fi
        return 1
    fi
}

check_absent() {
    local pattern="$1"
    if [[ -f "$LOG_FILE" ]] && grep -Fq "$pattern" "$LOG_FILE" 2>/dev/null; then
        echo "[FAIL] risk pattern found: $pattern"
        ((FAIL_COUNT++)) || true
    else
        echo "[OK] absent: $pattern"
    fi
}

echo "[info] checking static OpenGEM wiring..."
check_static "opengem  - launch OpenGEM GUI (preflight + run)" "$SHELL_FILE" "shell help exposes OpenGEM command"
check_static "[ app ] opengem launch requested" "$SHELL_FILE" "launch request marker present"
check_static "[ app ] opengem preflight started" "$SHELL_FILE" "preflight start marker present"
check_static "[ app ] opengem preflight entry: ok" "$SHELL_FILE" "entry-ok marker present"
check_static "[ app ] opengem preflight entry: missing" "$SHELL_FILE" "entry-missing marker present"
check_static "[ app ] opengem preflight fat: ok" "$SHELL_FILE" "FAT-ready marker present"
check_static "[ app ] opengem preflight fat: fail" "$SHELL_FILE" "FAT-fail marker present"
check_static "[ app ] opengem preflight complete" "$SHELL_FILE" "preflight completion marker present"
check_static "[ app ] opengem preflight failed" "$SHELL_FILE" "preflight failure marker present"
check_static "[ app ] opengem preflight passed" "$SHELL_FILE" "preflight success marker present"
check_static "shell_run(boot_info, handoff, found_path);" "$SHELL_FILE" "preflight success dispatches runnable entry"
check_static 'mcopy -s -o -i "$IMAGE" "$OPENGEM_RUNTIME_DIR"/* ::FREEDOS/OPENGEM/' "$RUN_FILE" "image builder copies OpenGEM runtime tree"
check_static "::FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP" "$IMAGE_PROBE_FILE" "image probe covers desktop app payload"

# === Check 1: Log file exists ===
if [[ ! -f "$LOG_FILE" ]]; then
    echo "[SKIP] No boot log found: $LOG_FILE"
    if [[ "$FAIL_COUNT" -gt 0 ]]; then
        echo "[FAIL] OpenGEM smoke test found $FAIL_COUNT static issue(s)"
        exit 1
    fi
    echo "[PASS] OpenGEM smoke test complete (SKIP - no boot log)"
    exit 0
fi

# === Check 2: OpenGEM payload presence ===
OPENGEM_PRESENT=0
if [[ -d "$OPENGEM_RUNTIME" ]]; then
    # Check for any runnable entry
    for cand in GEM.BAT GEM.EXE DESKTOP.APP OPENGEM.BAT OPENGEM.EXE; do
        if find "$OPENGEM_RUNTIME" -maxdepth 3 -iname "$cand" -type f 2>/dev/null | grep -q .; then
            echo "[info] OpenGEM payload: PRESENT at $OPENGEM_RUNTIME (entry: $cand)"
            OPENGEM_PRESENT=1
            break
        fi
    done
    if [[ "$OPENGEM_PRESENT" -eq 0 ]]; then
        echo "[info] OpenGEM payload: PRESENT but no runnable entry found"
    fi
else
    echo "[info] OpenGEM payload: ABSENT (skip semantics active)"
fi

# === Check 3: Shell command surface ===
echo ""
echo "[info] checking shell command surface..."
check_marker "opengem" "INFO" || true

# === Check 4: Boot integrity (no panics) ===
echo ""
echo "[info] checking boot integrity..."
check_absent "[ panic ]"
check_absent "Invalid Opcode"
check_absent "#UD"
check_absent "General Protection Fault"

# === Check 5: OpenGEM-specific markers (only if payload present) ===
echo ""
if [[ "$OPENGEM_PRESENT" -eq 1 ]]; then
    echo "[info] checking OpenGEM launch markers (payload present)..."
    check_marker "[ app ] opengem launch requested" "WARN" || true
    check_marker "[ app ] opengem preflight started" "WARN" || true
    check_marker "[ app ] opengem preflight complete" "WARN" || true
else
    echo "[info] OpenGEM launch markers: SKIP (payload absent, not expected in log)"
    check_marker "[ app ] opengem launch requested" "INFO" || true
    check_marker "[ app ] opengem preflight started" "INFO" || true
fi

# === Summary ===
echo ""
echo "=== OpenGEM integration summary ==="
echo "  Payload:         $( [[ $OPENGEM_PRESENT -eq 1 ]] && echo PRESENT || echo ABSENT )"
echo "  Boot integrity:  $( [[ $FAIL_COUNT -eq 0 ]] && echo OK || echo ISSUES )"
echo "==================================="
echo ""

if [[ "$FAIL_COUNT" -gt 0 ]]; then
    echo "[FAIL] OpenGEM smoke test found $FAIL_COUNT issue(s)"
    exit 1
fi

if [[ "$OPENGEM_PRESENT" -eq 0 ]]; then
    echo "[PASS] OpenGEM smoke test complete (SKIP - payload absent)"
else
    echo "[PASS] OpenGEM smoke test complete"
fi
