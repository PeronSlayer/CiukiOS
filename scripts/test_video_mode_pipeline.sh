#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/video-mode-pipeline.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE"

# Avoid parallel QEMU/image races with other boot tests.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE"
    if ! flock -w 180 9; then
        echo "[FAIL] could not acquire QEMU test lock: $LOCK_FILE" >&2
        exit 1
    fi
fi

if [[ ! -x "$RUN_SCRIPT" ]]; then
    echo "[FAIL] run script not found or not executable: $RUN_SCRIPT" >&2
    exit 1
fi

echo "[test-video-mode] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
rc=$?
set -e

if [[ $rc -eq 124 ]]; then
    echo "[test-video-mode] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "[video] mode=double-buffer"
    "[ video ] gop modes=0x"
    "[ video ] active mode=0x"
    "GOP: policy1024 available="
    "[ ok ] stage2 mini shell ready"
    "vmode"
)

forbidden_patterns=(
    "[ panic ]"
    "Invalid Opcode"
    "#UD"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] missing required pattern: $pattern" >&2
        tail -n 160 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] forbidden pattern found: $pattern" >&2
        tail -n 160 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] absent: $pattern"
done

policy_line="$(grep -F "GOP: policy1024" "$LOG_FILE" | tail -n 1 || true)"
if [[ -z "$policy_line" ]]; then
    echo "[FAIL] missing GOP runtime policy line" >&2
    tail -n 160 "$LOG_FILE" >&2 || true
    exit 1
fi

if [[ "$policy_line" != *"result=PASS"* ]]; then
    echo "[FAIL] GOP runtime policy reported failure: $policy_line" >&2
    tail -n 160 "$LOG_FILE" >&2 || true
    exit 1
fi
echo "[OK] runtime 1024 policy: $policy_line"

echo "[PASS] video mode pipeline test completed"
echo "[INFO] log: $LOG_FILE"
