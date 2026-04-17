#!/usr/bin/env bash
# test_doom_readiness_m6.sh - Aggregate gate for M6 protected-mode readiness.
# Verifies M6 infrastructure and no regressions to existing paths.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pass=0 fail=0

echo "=== M6 DOS Extender Readiness Gate ==="
echo

# Gate 1: M6 requirements doc
if [[ -f "$PROJECT_DIR/docs/m6-dos-extender-requirements.md" ]]; then
  echo "[PASS] M6 requirements doc"
  ((pass++))
else
  echo "[FAIL] M6 requirements doc"
  ((fail++))
fi

# Gate 2: Kernel structure
if [[ -d "$PROJECT_DIR/kernel/include" ]]; then
  echo "[PASS] kernel structure"
  ((pass++))
else
  echo "[FAIL] kernel structure"
  ((fail++))
fi

# Gate 3: Build passes
if cd "$PROJECT_DIR" && make all > /dev/null 2>&1; then
  echo "[PASS] make all"
  ((pass++))
else
  echo "[FAIL] make all"
  ((fail++))
fi

# Gate 4: Video 1024 compat (M1 baseline)
if bash "$PROJECT_DIR/scripts/test_video_1024_compat.sh" > /dev/null 2>&1; then
  echo "[PASS] video 1024 compat"
  ((pass++))
else
  echo "[FAIL] video 1024 compat"
  ((fail++))
fi

# Gate 5: MZ regression (M2 core)
if bash "$PROJECT_DIR/scripts/test_mz_regression.sh" > /dev/null 2>&1; then
  echo "[PASS] MZ regression"
  ((pass++))
else
  echo "[FAIL] MZ regression"
  ((fail++))
fi

echo
echo "=== Summary ==="
echo "PASS: $pass / FAIL: $fail"

if (( fail == 0 )); then
  echo "[PASS] M6 readiness gate passed"
  exit 0
else
  echo "[FAIL] M6 readiness gate failed"
  exit 1
fi
