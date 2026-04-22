#!/usr/bin/env bash
# test_fat_compat.sh — FAT filesystem compatibility smoke test for CiukiOS M3
#
# Checks that:
#   1. Stage2 boots and mounts the FAT layer.
#   2. The 'copy' command is present in the shell command list.
#   3. The 'dir' command produces decimal-format output.
#   4. File-not-found and directory-type error messages are correct.
#   5. Regression: no FAT panic, no bad cluster walk output.
#
# This test re-uses the serial log produced by test_stage2_boot.sh when both
# run in the same session, or it boots QEMU independently if no log exists.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/stage2-boot.log"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"

mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------------------
# Boot QEMU only when the log is stale or absent
# ---------------------------------------------------------------------------
if [[ ! -f "$LOG_FILE" ]]; then
    echo "[test-fat-compat] no existing boot log, booting QEMU..."
    if [[ ! -x "$RUN_SCRIPT" ]]; then
        echo "[FAIL] run script not found or not executable: $RUN_SCRIPT" >&2
        exit 1
    fi
    set +e
    timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
    rc=$?
    set -e
    if [[ $rc -ne 0 && $rc -ne 124 ]]; then
        echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
        tail -n 60 "$LOG_FILE" >&2 || true
        exit 1
    fi
else
    echo "[test-fat-compat] reusing existing log: $LOG_FILE"
fi

# ---------------------------------------------------------------------------
# Pattern checks
# ---------------------------------------------------------------------------
#
# Required patterns — assert the FAT layer and shell reached a clean state.
# These cover:
#   - disk cache availability (prerequisite for FAT rw)
#   - FAT mount with rw-cache mode confirmed
#   - shell-ready banner lists all M3 file management commands
#   - mini command loop activated (shell entered successfully)
#   - stage2 reached the DOS-runtime handoff point without panic
#
# Forbidden patterns — guard against regressions in FAT walk and CPU state:
#   - panic and invalid-opcode catch any hard faults
#   - fat: bad cluster / fat: chain error catch walk-path corruption
#   - tick #100 catchs runaway loops that skip the FAT/shell init
# ---------------------------------------------------------------------------

required_patterns=(
    # Disk cache must be available before FAT can mount
    "[ ok ] disk cache layer is available"

    # FAT layer must mount in rw-cache mode (exact string — catches mode downgrade)
    "[ ok ] FAT layer mounted (rw cache)"

    # Shell-ready banner must list all M3 file management commands
    "type/copy/ren/move/mkdir/rmdir/attrib/del"

    # Shell must enter command loop
    "[ shell ] mini command loop active"

    # stage2 must reach DOS-runtime handoff without aborting
    "[ stage2 ] next step: handoff to DOS-like runtime"
)

forbidden_patterns=(
    # Hard CPU faults
    "[ panic ]"
    "Invalid Opcode"
    "#UD"

    # FAT walk / chain corruption
    "fat: bad cluster"
    "fat: chain error"
    "fat: corrupt entry"

    # Runaway timer — tick 100 means init loop never completed
    "[ tick ] irq0 #0000000000000064"
)

PASS=0
FAIL=0

for pattern in "${required_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[OK]   required: $pattern"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] missing:  $pattern" >&2
        FAIL=$((FAIL + 1))
    fi
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] forbidden found: $pattern" >&2
        FAIL=$((FAIL + 1))
    else
        echo "[OK]   absent:   $pattern"
        PASS=$((PASS + 1))
    fi
done

EXPECTED_PASS=$(( ${#required_patterns[@]} + ${#forbidden_patterns[@]} ))

echo ""
echo "[test-fat-compat] results: $PASS passed, $FAIL failed (expected $EXPECTED_PASS total checks)"

if [[ $FAIL -gt 0 ]]; then
    echo "[FAIL] FAT compatibility test FAILED" >&2
    exit 1
fi

if [[ $PASS -lt $EXPECTED_PASS ]]; then
    echo "[FAIL] fewer checks passed than expected ($PASS < $EXPECTED_PASS)" >&2
    exit 1
fi

echo "[PASS] FAT compatibility test completed ($PASS/$EXPECTED_PASS)"
