#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat << 'TXT'
Usage: scripts/opengem_regression_lock.sh [--no-build] [--timeout-sec <n>] [--label <name>]

OG-P2-01 regression lock for known historical OpenGEM bugs.

Checks performed:
1) Full-profile smoke run with OpenGEM enabled
2) OpenGEM trace artifact generation
3) Static source invariants:
   - deterministic stage2 fallback guard (AX=0002 gate)
   - VDx alias mapping to SD driver path
   - find-next special-mode one-shot compatibility path
4) Runtime forbidden signatures:
   - no false I/O carry regression signature 3F:02
   - no memory free regression signature 49:09

Artifacts:
- build/full/opengem-regression-lock.<label>.report.txt
- build/full/opengem-trace-full.<label>.*
TXT
}

DO_BUILD=1
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-30}"
LABEL="latest"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --timeout-sec)
      TIMEOUT_SEC="${2:-}"
      [[ -n "$TIMEOUT_SEC" ]] || { echo "[opengem-reglock] ERROR: missing value for --timeout-sec" >&2; exit 1; }
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      [[ -n "$LABEL" ]] || { echo "[opengem-reglock] ERROR: missing value for --label" >&2; exit 1; }
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[opengem-reglock] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$TIMEOUT_SEC" -lt 1 ]]; then
  echo "[opengem-reglock] ERROR: timeout must be integer >= 1" >&2
  exit 1
fi

REPORT="build/full/opengem-regression-lock.${LABEL}.report.txt"
TRACE_SERIAL="build/full/opengem-trace-full.${LABEL}.serial.log"
TRACE_INT="build/full/opengem-trace-full.${LABEL}.qemu-int.log"
TRACE_SUMMARY="build/full/opengem-trace-full.${LABEL}.int21-summary.txt"
mkdir -p build/full
rm -f "$REPORT"

FAILS=0
PASS=()
FAIL=()

record_pass() {
  PASS+=("$1")
}

record_fail() {
  FAIL+=("$1")
  FAILS=$((FAILS + 1))
}

SMOKE_ARGS=(--test)
if [[ "$DO_BUILD" -eq 0 ]]; then
  SMOKE_ARGS+=(--no-build)
fi

echo "[opengem-reglock] step 1/3 full smoke"
set +e
CIUKIOS_INCLUDE_OPENGEM=1 CIUKIOS_STAGE2_AUTORUN=1 QEMU_TIMEOUT_SEC="$TIMEOUT_SEC" bash scripts/qemu_run_full.sh "${SMOKE_ARGS[@]}"
SMOKE_RC=$?
set -e
if [[ $SMOKE_RC -eq 0 ]]; then
  record_pass "full_smoke_with_opengem"
else
  record_fail "full_smoke_with_opengem(rc=$SMOKE_RC)"
fi

echo "[opengem-reglock] step 2/3 trace artifacts"
TRACE_ARGS=(--label "$LABEL" --timeout-sec "$TIMEOUT_SEC")
if [[ "$DO_BUILD" -eq 0 ]]; then
  TRACE_ARGS+=(--no-build)
fi
if CIUKIOS_STAGE2_AUTORUN=1 bash scripts/opengem_trace_full.sh "${TRACE_ARGS[@]}"; then
  record_pass "trace_generation"
else
  record_fail "trace_generation"
fi

if [[ -f "$TRACE_SERIAL" && -f "$TRACE_INT" && -f "$TRACE_SUMMARY" ]]; then
  record_pass "trace_artifacts_present"
else
  record_fail "trace_artifacts_present"
fi

echo "[opengem-reglock] step 3/3 invariant checks"

# Deterministic fallback guards in stage2 (continue only on AX=0002).
FALLBACK_GUARDS="$( (rg -n "cmp ax, 0x0002" src/boot/full_stage2.asm || true) | wc -l | tr -d '[:space:]' )"
if [[ "$FALLBACK_GUARDS" -ge 2 ]]; then
  record_pass "stage2_fallback_guards"
else
  record_fail "stage2_fallback_guards(found=$FALLBACK_GUARDS)"
fi

# Alias mapping: VDx wildcard probes mapped to SD driver path.
if rg -n "OpenGEM probes VDx wildcard names|path_sd_driver_fat" src/boot/floppy_stage1.asm >/dev/null; then
  record_pass "vd_alias_to_sd_mapping"
else
  record_fail "vd_alias_to_sd_mapping"
fi

# Find-next one-shot special mode compatibility path.
if rg -n "find_special_mode|mov byte \[cs:find_special_mode\], 0|xor ax, ax" src/boot/floppy_stage1.asm >/dev/null; then
  record_pass "findnext_special_mode_oneshot"
else
  record_fail "findnext_special_mode_oneshot"
fi

if [[ -f "$TRACE_SERIAL" ]]; then
  if rg -n "(\[IERR\].*3F:02|\[\[IIEERRRR\]\].*33FF::0022)" "$TRACE_SERIAL" >/dev/null; then
    record_fail "forbidden_signature_3F_02"
  else
    record_pass "forbidden_signature_3F_02_absent"
  fi

  if rg -n "(\[IERR\].*49:09|\[\[IIEERRRR\]\].*4499::0099)" "$TRACE_SERIAL" >/dev/null; then
    record_fail "forbidden_signature_49_09"
  else
    record_pass "forbidden_signature_49_09_absent"
  fi

  if rg -n "(\[OPENGEM\].*launch|OOPPEENNGGEEMM.*llaauunncch)" "$TRACE_SERIAL" >/dev/null; then
    record_pass "launch_marker_present"
  else
    record_fail "launch_marker_present"
  fi

  if rg -n "(\[OPENGEM\].*try GEMVDI|GGEEMMVVDDI{1,2})" "$TRACE_SERIAL" >/dev/null; then
    record_pass "gemvdi_probe_marker_present"
  else
    record_fail "gemvdi_probe_marker_present"
  fi
else
  record_fail "trace_serial_missing"
fi

{
  echo "OpenGEM Regression Lock Report (OG-P2-01)"
  echo "Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  echo "Label: $LABEL"
  echo
  echo "Passed checks: ${#PASS[@]}"
  for p in "${PASS[@]}"; do
    echo "- PASS: $p"
  done
  echo
  echo "Failed checks: ${#FAIL[@]}"
  for f in "${FAIL[@]}"; do
    echo "- FAIL: $f"
  done
  echo
  echo "Artifacts:"
  echo "- $TRACE_SERIAL"
  echo "- $TRACE_INT"
  echo "- $TRACE_SUMMARY"
} > "$REPORT"

echo "[opengem-reglock] report: $REPORT"
if [[ $FAILS -eq 0 ]]; then
  echo "[opengem-reglock] PASS"
  exit 0
fi

echo "[opengem-reglock] FAIL" >&2
exit 1
