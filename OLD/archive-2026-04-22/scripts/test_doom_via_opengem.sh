#!/usr/bin/env bash
# scripts/test_doom_via_opengem.sh — OPENGEM-006 DOOM Path Readiness.
#
# Fixture-gated end-to-end harness. When CIUKIOS_DOOM_FIXTURES_DIR is
# unset or empty, the harness SKIPs cleanly (DOOM shareware is user-
# supplied per licensing policy and is not redistributed by the
# project). When set, it asserts:
#   1. Stage2 catalog discovered DOOM.EXE / DOOM1.WAD.
#   2. OpenGEM launch emitted the DOOM launch marker.
#   3. DOOM boot reached the menu stage.
#
# The harness also validates the static source invariants that the
# required markers exist in stage2 and that the catalog probe is in
# place, so basic coverage is maintained even when no fixtures exist.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE2_C="$ROOT/stage2/src/stage2.c"
SHELL_C="$ROOT/stage2/src/shell.c"
BOOT_LOG="${CIUKIOS_DOOM_BOOT_LOG:-$ROOT/.ciukios-testlogs/doom-boot.log}"

OK=0
FAIL=0
ok() { echo "[OK] $1"; OK=$((OK+1)); }
ko() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

check_contains() {
    local needle="$1" path="$2" label="$3"
    if grep -qF -- "$needle" "$path"; then ok "$label"
    else ko "$label (missing: $needle)"; fi
}

echo "=== OPENGEM-006 DOOM path readiness ==="

# --- Static invariants (always run) ---
check_contains 'OPENGEM-006' "$STAGE2_C" "stage2.c: OPENGEM-006 probe present"
check_contains 'catalog discovered DOOM.EXE at' "$STAGE2_C" \
    "stage2.c: DOOM.EXE discovery marker"
check_contains 'catalog discovered DOOM1.WAD at' "$STAGE2_C" \
    "stage2.c: DOOM1.WAD discovery marker"
check_contains 'app_catalog_find("DOOM.EXE")' "$STAGE2_C" \
    "stage2.c: DOOM.EXE catalog lookup"
check_contains 'app_catalog_find("DOOM1.WAD")' "$STAGE2_C" \
    "stage2.c: DOOM1.WAD catalog lookup"
check_contains 'OPENGEM-006' "$SHELL_C" "shell.c: OPENGEM-006 launch hook"
check_contains '[ doom ] opengem launch DOOM.EXE' "$SHELL_C" \
    "shell.c: DOOM launch marker"

# --- Fixture-gated runtime assertions ---
if [ -z "${CIUKIOS_DOOM_FIXTURES_DIR:-}" ]; then
    echo "[SKIP] CIUKIOS_DOOM_FIXTURES_DIR not set - runtime DOOM markers not checked"
    echo "[info] to run the full gate: export CIUKIOS_DOOM_FIXTURES_DIR=/path/to/user/doom/fixtures"
    echo ""
    echo "=== doom-via-opengem summary: $OK OK / $FAIL FAIL (fixtures SKIP) ==="
    if [ "$FAIL" -gt 0 ]; then
        echo "[FAIL] DOOM-via-OpenGEM static gate"
        exit 1
    fi
    echo "[PASS] DOOM-via-OpenGEM static gate (fixtures SKIPPED)"
    exit 0
fi

echo "[info] CIUKIOS_DOOM_FIXTURES_DIR=$CIUKIOS_DOOM_FIXTURES_DIR"

if [ ! -d "$CIUKIOS_DOOM_FIXTURES_DIR" ]; then
    ko "fixtures directory does not exist: $CIUKIOS_DOOM_FIXTURES_DIR"
fi

if [ ! -f "$BOOT_LOG" ]; then
    ko "DOOM boot log not found at $BOOT_LOG (set CIUKIOS_DOOM_BOOT_LOG to override)"
else
    ok "boot log present: $BOOT_LOG"
    for m in \
        '[ doom ] catalog discovered DOOM.EXE at' \
        '[ doom ] catalog discovered DOOM1.WAD at' \
        '[ doom ] opengem launch DOOM.EXE' \
        '[ doom ] stage reached: menu'
    do
        if grep -qF -- "$m" "$BOOT_LOG"; then
            ok "runtime marker: '$m'"
        else
            ko "runtime marker missing: '$m'"
        fi
    done
fi

echo ""
echo "=== doom-via-opengem summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] DOOM-via-OpenGEM end-to-end gate"
    exit 1
fi
echo "[PASS] DOOM-via-OpenGEM end-to-end gate"
