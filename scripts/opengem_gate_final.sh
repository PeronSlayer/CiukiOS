#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat << 'TXT'
Usage: scripts/opengem_gate_final.sh [options]

Runs the official OG-P0-05 final gate by chaining:
1) Full-profile smoke gate
2) OpenGEM trace artifacts
3) OpenGEM acceptance campaign

Options:
  --runs <n>                  Acceptance runs (default: RUNS env or 20)
  --timeout-sec <n>           Timeout per run in seconds (default: QEMU_TIMEOUT_SEC env or 30)
  --label <name>              Artifact label suffix (default: latest)
  --no-build                  Reuse existing image and skip rebuild in trace/acceptance
  --skip-smoke                Skip full-profile smoke gate step
  --launch-threshold <pct>    Min launch success percent (default: 90)
  --return-threshold <pct>    Min return-to-shell percent (default: 95)
  --max-hangs <n>             Max accepted hangs (default: derived from return threshold)

Environment overrides:
  RUNS
  QEMU_TIMEOUT_SEC
  OPENGEM_GATE_LAUNCH_THRESHOLD
  OPENGEM_GATE_RETURN_THRESHOLD
  OPENGEM_GATE_MAX_HANGS
TXT
}

RUNS="${RUNS:-20}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-30}"
LABEL="latest"
DO_BUILD=1
DO_SMOKE=1
LAUNCH_THRESHOLD="${OPENGEM_GATE_LAUNCH_THRESHOLD:-90}"
RETURN_THRESHOLD="${OPENGEM_GATE_RETURN_THRESHOLD:-95}"
MAX_HANGS="${OPENGEM_GATE_MAX_HANGS:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)
      RUNS="${2:-}"
      [[ -n "$RUNS" ]] || { echo "[opengem-gate] ERROR: missing value for --runs" >&2; exit 1; }
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC="${2:-}"
      [[ -n "$TIMEOUT_SEC" ]] || { echo "[opengem-gate] ERROR: missing value for --timeout-sec" >&2; exit 1; }
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      [[ -n "$LABEL" ]] || { echo "[opengem-gate] ERROR: missing value for --label" >&2; exit 1; }
      shift 2
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --skip-smoke)
      DO_SMOKE=0
      shift
      ;;
    --launch-threshold)
      LAUNCH_THRESHOLD="${2:-}"
      [[ -n "$LAUNCH_THRESHOLD" ]] || { echo "[opengem-gate] ERROR: missing value for --launch-threshold" >&2; exit 1; }
      shift 2
      ;;
    --return-threshold)
      RETURN_THRESHOLD="${2:-}"
      [[ -n "$RETURN_THRESHOLD" ]] || { echo "[opengem-gate] ERROR: missing value for --return-threshold" >&2; exit 1; }
      shift 2
      ;;
    --max-hangs)
      MAX_HANGS="${2:-}"
      [[ -n "$MAX_HANGS" ]] || { echo "[opengem-gate] ERROR: missing value for --max-hangs" >&2; exit 1; }
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[opengem-gate] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "$RUNS" =~ ^[0-9]+$ ]] || [[ "$RUNS" -lt 1 ]]; then
  echo "[opengem-gate] ERROR: --runs must be an integer >= 1" >&2
  exit 1
fi

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$TIMEOUT_SEC" -lt 1 ]]; then
  echo "[opengem-gate] ERROR: --timeout-sec must be an integer >= 1" >&2
  exit 1
fi

if ! [[ "$LAUNCH_THRESHOLD" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "[opengem-gate] ERROR: --launch-threshold must be numeric" >&2
  exit 1
fi

if ! [[ "$RETURN_THRESHOLD" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "[opengem-gate] ERROR: --return-threshold must be numeric" >&2
  exit 1
fi

if [[ -z "$MAX_HANGS" ]]; then
  MAX_HANGS="$(awk -v runs="$RUNS" -v ret="$RETURN_THRESHOLD" 'BEGIN { req = int((ret*runs + 99.999)/100.0); maxh = runs - req; if (maxh < 0) maxh = 0; printf "%d", maxh }')"
fi

if ! [[ "$MAX_HANGS" =~ ^[0-9]+$ ]]; then
  echo "[opengem-gate] ERROR: --max-hangs must be an integer >= 0" >&2
  exit 1
fi

mkdir -p build/full
GATE_REPORT="build/full/opengem-gate-final.${LABEL}.report.txt"
rm -f "$GATE_REPORT"

SMOKE_STATUS="SKIPPED"
SMOKE_RC=0
if [[ "$DO_SMOKE" -eq 1 ]]; then
  echo "[opengem-gate] step 1/3: full smoke gate"
  SMOKE_ARGS=(--test)
  if [[ "$DO_BUILD" -eq 0 ]]; then
    SMOKE_ARGS+=(--no-build)
  fi
  set +e
  CIUKIOS_INCLUDE_OPENGEM=1 QEMU_TIMEOUT_SEC="$TIMEOUT_SEC" bash scripts/qemu_run_full.sh "${SMOKE_ARGS[@]}"
  SMOKE_RC=$?
  set -e
  if [[ $SMOKE_RC -eq 0 ]]; then
    SMOKE_STATUS="PASS"
  else
    SMOKE_STATUS="FAIL"
  fi
fi

echo "[opengem-gate] step 2/3: trace artifacts"
TRACE_ARGS=(--label "$LABEL" --timeout-sec "$TIMEOUT_SEC")
if [[ "$DO_BUILD" -eq 0 ]]; then
  TRACE_ARGS+=(--no-build)
fi
bash scripts/opengem_trace_full.sh "${TRACE_ARGS[@]}"

echo "[opengem-gate] step 3/3: acceptance campaign"
ACC_ARGS=(--label "$LABEL" --timeout-sec "$TIMEOUT_SEC" --runs "$RUNS")
if [[ "$DO_BUILD" -eq 0 ]]; then
  ACC_ARGS+=(--no-build)
fi
bash scripts/opengem_acceptance_full.sh "${ACC_ARGS[@]}"

ACC_REPORT="build/full/opengem-acceptance-full.${LABEL}.report.txt"
TRACE_SERIAL="build/full/opengem-trace-full.${LABEL}.serial.log"
TRACE_INT="build/full/opengem-trace-full.${LABEL}.qemu-int.log"
TRACE_SUMMARY="build/full/opengem-trace-full.${LABEL}.int21-summary.txt"

for f in "$ACC_REPORT" "$TRACE_SERIAL" "$TRACE_INT" "$TRACE_SUMMARY"; do
  if [[ ! -f "$f" ]]; then
    echo "[opengem-gate] FAIL: missing artifact $f" >&2
    exit 1
  fi
done

launch_rate="$(awk -F': ' '/launch_success_rate_percent/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
return_rate="$(awk -F': ' '/return_to_shell_rate_percent/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
hang_count="$(awk -F': ' '/hang_count/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
avg_latency="$(awk -F': ' '/average_launch_latency_sec/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"

if [[ -z "$launch_rate" || -z "$return_rate" || -z "$hang_count" ]]; then
  echo "[opengem-gate] FAIL: incomplete acceptance metrics in $ACC_REPORT" >&2
  exit 1
fi

launch_ok="$(awk -v v="$launch_rate" -v t="$LAUNCH_THRESHOLD" 'BEGIN { if (v+0 >= t+0) print 1; else print 0 }')"
return_ok="$(awk -v v="$return_rate" -v t="$RETURN_THRESHOLD" 'BEGIN { if (v+0 >= t+0) print 1; else print 0 }')"
hang_ok=0
if [[ "$hang_count" =~ ^[0-9]+$ ]] && [[ "$hang_count" -le "$MAX_HANGS" ]]; then
  hang_ok=1
fi

verdict="PASS"
if [[ "$launch_ok" -ne 1 || "$return_ok" -ne 1 || "$hang_ok" -ne 1 ]]; then
  verdict="FAIL"
fi
if [[ "$DO_SMOKE" -eq 1 && "$SMOKE_STATUS" != "PASS" ]]; then
  verdict="FAIL"
fi

cat > "$GATE_REPORT" << EOF
OpenGEM OG-P0-05 Final Gate Report
Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Label: $LABEL
Verdict: $verdict

Inputs:
- Runs: $RUNS
- TimeoutSec: $TIMEOUT_SEC
- LaunchThresholdPercent: $LAUNCH_THRESHOLD
- ReturnThresholdPercent: $RETURN_THRESHOLD
- MaxHangs: $MAX_HANGS

Step status:
- Full smoke gate: $SMOKE_STATUS

Acceptance metrics:
- launch_success_rate_percent: $launch_rate
- return_to_shell_rate_percent: $return_rate
- hang_count: $hang_count
- average_launch_latency_sec: $avg_latency

Checks:
- launch_threshold_check: $launch_ok
- return_threshold_check: $return_ok
- max_hangs_check: $hang_ok

Artifacts:
- $ACC_REPORT
- $TRACE_SERIAL
- $TRACE_INT
- $TRACE_SUMMARY
EOF

echo "[opengem-gate] final report: $GATE_REPORT"
if [[ "$verdict" == "PASS" ]]; then
  echo "[opengem-gate] PASS: OG-P0-05 gate criteria satisfied"
  exit 0
fi

echo "[opengem-gate] FAIL: OG-P0-05 gate criteria not satisfied" >&2
exit 1
