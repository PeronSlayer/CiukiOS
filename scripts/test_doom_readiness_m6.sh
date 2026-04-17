#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_gate() {
    local name="$1"
    shift
    echo "[gate] $name ..."
    "$@"
    echo "[PASS] $name"
}

run_optional_gate() {
    local name="$1"
    shift
    echo "[gate] $name ..."
    if "$@"; then
        echo "[PASS] $name"
        return 0
    fi
    echo "[WARN] $name non-blocking failure for M6 readiness baseline"
}

echo "=== M6 DOS Extender Readiness Gate ==="

if [[ ! -f "$PROJECT_DIR/docs/m6-dos-extender-requirements.md" ]]; then
    echo "[FAIL] missing requirements doc: docs/m6-dos-extender-requirements.md" >&2
    exit 1
fi

mkdir -p "$PROJECT_DIR/.ciukios-testlogs"

cd "$PROJECT_DIR"
run_gate "test-phase2" make test-phase2
run_optional_gate "test-freedos-pipeline" make test-freedos-pipeline
run_gate "test-video-1024" make test-video-1024
run_gate "test-video-backbuf-policy" bash ./scripts/test_video_backbuf_policy.sh
run_gate "test-m6-pmode" make test-m6-pmode
run_gate "test-m6-transition-v2" bash ./scripts/test_m6_transition_contract_v2.sh
run_gate "test-m6-smoke" make test-m6-smoke
run_gate "test-m6-dos4gw-smoke" make test-m6-dos4gw-smoke

echo "[PASS] M6 readiness gate passed"
