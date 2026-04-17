#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/stage2-boot.log"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"

mkdir -p "$LOG_DIR"

if [[ ! -f "$LOG_FILE" ]]; then
    echo "[test-fat32-progress] no existing boot log, booting QEMU..."
    set +e
    timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
    rc=$?
    set -e
    if [[ $rc -ne 0 && $rc -ne 124 ]]; then
        echo "[FAIL] run_ciukios.sh failed (exit=$rc)" >&2
        tail -n 80 "$LOG_FILE" >&2 || true
        exit 1
    fi
else
    echo "[test-fat32-progress] reusing existing log: $LOG_FILE"
fi

if ! grep -Fq "[ fat ] mounted type=" "$LOG_FILE"; then
    echo "[FAIL] FAT mount marker with filesystem type not found" >&2
    exit 1
fi

if grep -Fq "[ fat ] mounted type=FAT32" "$LOG_FILE"; then
    if ! grep -Fq "fsinfo=" "$LOG_FILE"; then
        echo "[FAIL] FAT32 marker missing fsinfo status" >&2
        exit 1
    fi
    if ! grep -Fq "next_free_hint=0x" "$LOG_FILE"; then
        echo "[FAIL] FAT32 marker missing next_free_hint" >&2
        exit 1
    fi
    if ! grep -Fq "free_clusters=" "$LOG_FILE"; then
        echo "[FAIL] FAT32 marker missing free_clusters field" >&2
        exit 1
    fi
    echo "[OK] FAT32 marker includes FSInfo + next_free_hint + free_clusters"
else
    echo "[OK] FAT marker found (non-FAT32 media in current run)"
fi

if grep -Fq "[ warn ] FAT layer not mounted" "$LOG_FILE"; then
    echo "[FAIL] FAT layer reported as not mounted" >&2
    exit 1
fi

echo "[PASS] FAT32 progress test completed"
