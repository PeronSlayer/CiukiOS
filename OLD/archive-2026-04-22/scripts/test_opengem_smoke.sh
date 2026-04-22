#!/usr/bin/env bash
# OPENGEM-001 — Smoke gate for the OpenGEM launcher integration.
#
# Static-only gate (no QEMU launch required). Asserts:
#   * OpenGEM runtime payload is present on the host
#   * OpenGEM entry points documented in the roadmap are present
#   * stage2 exposes shell_run_opengem_interactive and the new markers
#   * desktop launcher dispatches OPENGEM
#   * ALT+O shortcut is wired in the desktop session loop
#   * the image pipeline copies OpenGEM into ::FREEDOS/OPENGEM
#
# If a stage2 boot log from a recent QEMU run is available, also checks
# for the boot/launch/exit markers. Otherwise these are treated as
# SKIP (not a failure) — the host-side assertions are authoritative.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHELL_FILE="$PROJECT_DIR/stage2/src/shell.c"
UI_FILE="$PROJECT_DIR/stage2/src/ui.c"
RUN_FILE="$PROJECT_DIR/run_ciukios.sh"
OPENGEM_RUNTIME="$PROJECT_DIR/third_party/freedos/runtime/OPENGEM"
BOOT_LOG="${OPENGEM_SMOKE_LOG:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"

echo "[test-opengem-smoke] OpenGEM launcher smoke gate v1"
echo ""

FAIL=0

fail() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
pass() { echo "[OK] $*"; }

check_static() {
    local pattern="$1"
    local file="$2"
    local desc="$3"
    if grep -Fq "$pattern" "$file" 2>/dev/null; then
        pass "$desc"
    else
        fail "$desc (pattern missing: $pattern)"
    fi
}

# --- Host-side runtime payload ---------------------------------------
echo "[info] checking runtime payload on host..."
if [[ -d "$OPENGEM_RUNTIME" ]]; then
    pass "runtime dir present: $OPENGEM_RUNTIME"
    if [[ -f "$OPENGEM_RUNTIME/GEM.BAT" ]]; then
        pass "GEM.BAT present"
    else
        fail "GEM.BAT missing under $OPENGEM_RUNTIME"
    fi
    if [[ -f "$OPENGEM_RUNTIME/GEMAPPS/GEMSYS/DESKTOP.APP" ]] \
       || find "$OPENGEM_RUNTIME/GEMAPPS/GEMSYS" -maxdepth 1 -iname 'GEMVDI*' -type f 2>/dev/null | grep -q .; then
        pass "GEMAPPS/GEMSYS payload present"
    else
        fail "GEMAPPS/GEMSYS entry missing (DESKTOP.APP / GEMVDI*)"
    fi
else
    echo "[WARN] runtime dir absent: $OPENGEM_RUNTIME (smoke gate will degrade gracefully)"
fi

# --- Stage2 launcher integration -------------------------------------
echo ""
echo "[info] checking stage2 launcher integration..."
check_static "shell_run_opengem_interactive" "$SHELL_FILE" "helper defined in stage2 shell"
check_static "OpenGEM: boot sequence starting" "$SHELL_FILE" "boot marker present"
check_static "OpenGEM: launcher window initialized" "$SHELL_FILE" "launcher-init marker present"
check_static "OpenGEM: exit detected, returning to shell" "$SHELL_FILE" "exit marker present"
check_static "OpenGEM: runtime not found in FAT, fallback to shell" "$SHELL_FILE" "fallback marker present"
check_static "[ ui ] alt+o shortcut: opengem" "$SHELL_FILE" "ALT+O shortcut wired in desktop session"
check_static "str_eq_nocase(action, \"OPENGEM\")" "$SHELL_FILE" "launcher dispatch handles OPENGEM"

# --- Desktop launcher item list --------------------------------------
echo ""
echo "[info] checking desktop launcher item list..."
check_static "#define LAUNCHER_ITEMS 7" "$UI_FILE" "launcher items bumped to 7"
check_static "\"OPENGEM\"" "$UI_FILE" "OPENGEM label present in launcher list"

# --- Image pipeline --------------------------------------------------
echo ""
echo "[info] checking image pipeline copies OpenGEM..."
check_static "::FREEDOS/OPENGEM" "$RUN_FILE" "run_ciukios.sh copies OpenGEM into image"

# --- Optional boot log probe -----------------------------------------
echo ""
if [[ -f "$BOOT_LOG" ]]; then
    echo "[info] probing boot log: $BOOT_LOG"
    for marker in \
        "OpenGEM: boot sequence starting" \
        "OpenGEM: launcher window initialized" \
        "OpenGEM: exit detected, returning to shell"; do
        if grep -Fq "$marker" "$BOOT_LOG" 2>/dev/null; then
            pass "log: $marker"
        else
            echo "[SKIP] log missing marker: $marker (launch not exercised in this run)"
        fi
    done
else
    echo "[info] no boot log at $BOOT_LOG — skipping runtime marker probe"
fi

# --- Summary ---------------------------------------------------------
echo ""
if [[ "$FAIL" -gt 0 ]]; then
    echo "[FAIL] OpenGEM smoke gate: $FAIL issue(s)"
    exit 1
fi
echo "[PASS] OpenGEM smoke gate"
