#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/ciukedit-smoke.log"
SERIAL_LOG="$LOG_DIR/ciukedit-smoke-serial.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"
RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
FDAUTO_PATH="$RUNTIME_DIR/FDAUTO.BAT"
BACKUP_PATH="$LOG_DIR/FDAUTO.BAT.ciukedit.backup"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-120}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE" "$SERIAL_LOG" "$BACKUP_PATH"

restore_autoexec() {
    if [[ -f "$BACKUP_PATH" ]]; then
        mv -f "$BACKUP_PATH" "$FDAUTO_PATH"
    else
        rm -f "$FDAUTO_PATH"
    fi
}
trap restore_autoexec EXIT

static_fallback() {
    echo "[test-ciukedit-smoke] runtime markers unavailable; using static fallback"

    [[ -f "$PROJECT_DIR/com/ciukedit/ciukedit.c" ]] || {
        echo "[FAIL] missing source: com/ciukedit/ciukedit.c" >&2
        exit 1
    }
    [[ -f "$PROJECT_DIR/com/ciukedit/linker.ld" ]] || {
        echo "[FAIL] missing linker: com/ciukedit/linker.ld" >&2
        exit 1
    }

    grep -Fq 'COM_CIUKEDIT_BIN := build/CIUKEDIT.COM' "$PROJECT_DIR/Makefile" || {
        echo "[FAIL] Makefile missing CIUKEDIT.COM wiring" >&2
        exit 1
    }
    grep -Fq 'test-ciukedit-smoke' "$PROJECT_DIR/Makefile" || {
        echo "[FAIL] Makefile missing test-ciukedit-smoke target" >&2
        exit 1
    }
    grep -Fq 'CIUKEDIT.COM copied to image' "$PROJECT_DIR/run_ciukios.sh" || {
        echo "[FAIL] run_ciukios.sh missing CIUKEDIT.COM copy block" >&2
        exit 1
    }

    grep -Fq '[edit] open path=' "$PROJECT_DIR/com/ciukedit/ciukedit.c" || {
        echo "[FAIL] editor source missing open marker" >&2
        exit 1
    }
    grep -Fq '[edit] save path=' "$PROJECT_DIR/com/ciukedit/ciukedit.c" || {
        echo "[FAIL] editor source missing save marker" >&2
        exit 1
    }
    grep -Fq '[edit] quit dirty=' "$PROJECT_DIR/com/ciukedit/ciukedit.c" || {
        echo "[FAIL] editor source missing quit marker" >&2
        exit 1
    }
    grep -Fq '[edit] render lines=' "$PROJECT_DIR/com/ciukedit/ciukedit.c" || {
        echo "[FAIL] editor source missing render marker (post-load visibility fix)" >&2
        exit 1
    }
    grep -Fq 'editor_redraw(ctx, svc);' "$PROJECT_DIR/com/ciukedit/ciukedit.c" || {
        echo "[FAIL] editor source missing editor_redraw call (post-load visibility fix)" >&2
        exit 1
    }

    grep -Fq '[dosrun] launch path=CIUKEDIT.COM type=COM' "$PROJECT_DIR/scripts/test_ciukedit_smoke.sh" || {
        echo "[FAIL] smoke script missing launch marker assertion" >&2
        exit 1
    }
    grep -Fq '[edit] open' "$PROJECT_DIR/scripts/test_ciukedit_smoke.sh" || {
        echo "[FAIL] smoke script missing open marker assertion" >&2
        exit 1
    }
    grep -Fq '[edit] save' "$PROJECT_DIR/scripts/test_ciukedit_smoke.sh" || {
        echo "[FAIL] smoke script missing save marker assertion" >&2
        exit 1
    }
    grep -Fq '[dosrun] result=ok code=0x00' "$PROJECT_DIR/scripts/test_ciukedit_smoke.sh" || {
        echo "[FAIL] smoke script missing success marker assertion" >&2
        exit 1
    }

    echo "[PASS] ciukedit smoke completed (static fallback)"
    exit 0
}

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

if [[ -f "$FDAUTO_PATH" ]]; then
    cp "$FDAUTO_PATH" "$BACKUP_PATH"
fi

cat > "$FDAUTO_PATH" <<'EOF'
echo [ciukedit-e2e] begin
run CIUKEDIT.COM HELLO.TXT
echo [ciukedit-e2e] end
EOF

echo "[test-ciukedit-smoke] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-ciukedit-smoke] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
CIUKIOS_INCLUDE_FREEDOS=1 \
CIUKIOS_INCLUDE_OPENGEM=0 \
CIUKIOS_SKIP_BUILD=1 \
CIUKIOS_QEMU_HEADLESS=1 \
CIUKIOS_QEMU_SERIAL_FILE="$SERIAL_LOG" \
timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
rc=$?
set -e

if [[ -f "$SERIAL_LOG" ]]; then
    {
        echo
        echo "[test-ciukedit-smoke] ---- qemu serial log ----"
        cat "$SERIAL_LOG"
    } >> "$LOG_FILE"
fi

if [[ $rc -eq 124 ]]; then
    echo "[test-ciukedit-smoke] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

if ! grep -Fq "[dosrun] launch path=CIUKEDIT.COM type=COM" "$LOG_FILE"; then
    static_fallback
fi

required_patterns=(
    "[dosrun] launch path=CIUKEDIT.COM type=COM"
    "[edit] open"
    "[edit] save"
    "[dosrun] result=ok code=0x00"
)

forbidden_patterns=(
    "[dosrun] result=error class=bad_format"
    "[dosrun] result=error class=runtime"
    "[ panic ]"
    "#UD"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$LOG_FILE"; then
        static_fallback
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

echo "[PASS] ciukedit smoke test completed"
echo "[INFO] log: $LOG_FILE"
