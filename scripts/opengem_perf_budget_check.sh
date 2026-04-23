#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat << 'TXT'
Usage: scripts/opengem_perf_budget_check.sh [options]

Runs OG-P2-02 periodic budget check against a baseline.

Options:
  --baseline <path>     Baseline JSON (default: build/full/opengem-performance-baseline.latest.json)
  --budget <path>       Budget JSON (default: docs/opengem-performance-budget.json)
  --runs <n>            Current run count (default: RUNS env or 10)
  --timeout-sec <n>     Timeout per run (default: QEMU_TIMEOUT_SEC env or 20)
  --label <name>        Label for current artifacts (default: perfcheck)
  --no-build            Skip rebuild

Outputs:
  - build/full/opengem-performance-budget-check.<label>.report.txt
TXT
}

BASELINE="build/full/opengem-performance-baseline.latest.json"
BUDGET="docs/opengem-performance-budget.json"
RUNS="${RUNS:-10}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-20}"
LABEL="perfcheck"
DO_BUILD=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline)
      BASELINE="${2:-}"
      [[ -n "$BASELINE" ]] || { echo "[opengem-perf-check] ERROR: missing value for --baseline" >&2; exit 1; }
      shift 2
      ;;
    --budget)
      BUDGET="${2:-}"
      [[ -n "$BUDGET" ]] || { echo "[opengem-perf-check] ERROR: missing value for --budget" >&2; exit 1; }
      shift 2
      ;;
    --runs)
      RUNS="${2:-}"
      [[ -n "$RUNS" ]] || { echo "[opengem-perf-check] ERROR: missing value for --runs" >&2; exit 1; }
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC="${2:-}"
      [[ -n "$TIMEOUT_SEC" ]] || { echo "[opengem-perf-check] ERROR: missing value for --timeout-sec" >&2; exit 1; }
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      [[ -n "$LABEL" ]] || { echo "[opengem-perf-check] ERROR: missing value for --label" >&2; exit 1; }
      shift 2
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[opengem-perf-check] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -f "$BASELINE" ]]; then
  echo "[opengem-perf-check] ERROR: baseline not found: $BASELINE" >&2
  exit 1
fi

if [[ ! -f "$BUDGET" ]]; then
  echo "[opengem-perf-check] ERROR: budget config not found: $BUDGET" >&2
  exit 1
fi

if ! [[ "$RUNS" =~ ^[0-9]+$ ]] || [[ "$RUNS" -lt 1 ]]; then
  echo "[opengem-perf-check] ERROR: --runs must be integer >= 1" >&2
  exit 1
fi

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$TIMEOUT_SEC" -lt 1 ]]; then
  echo "[opengem-perf-check] ERROR: --timeout-sec must be integer >= 1" >&2
  exit 1
fi

extract_json_number() {
  local file="$1"
  local key="$2"
  sed -n -E "s/^[[:space:]]*\"$key\"[[:space:]]*:[[:space:]]*([0-9]+(\.[0-9]+)?).*/\1/p" "$file" | head -n 1
}

base_launch="$(extract_json_number "$BASELINE" "launch_success_rate_percent")"
base_return="$(extract_json_number "$BASELINE" "return_to_shell_rate_percent")"
base_ready="$(extract_json_number "$BASELINE" "desktop_ready_rate_percent")"
base_hang="$(extract_json_number "$BASELINE" "hang_count")"
base_latency="$(extract_json_number "$BASELINE" "average_launch_latency_sec")"
base_paras="$(extract_json_number "$BASELINE" "observed_max_allocated_paras")"

if [[ -z "$base_launch" || -z "$base_return" || -z "$base_hang" || -z "$base_latency" ]]; then
  echo "[opengem-perf-check] ERROR: incomplete baseline metrics in $BASELINE" >&2
  exit 1
fi

if [[ -z "$base_ready" ]]; then
  base_ready="$base_launch"
fi

latency_mult="$(extract_json_number "$BUDGET" "max_avg_launch_latency_multiplier")"
launch_drop="$(extract_json_number "$BUDGET" "max_launch_success_drop_percent")"
return_drop="$(extract_json_number "$BUDGET" "max_return_to_shell_drop_percent")"
ready_drop="$(extract_json_number "$BUDGET" "max_desktop_ready_drop_percent")"
hang_delta="$(extract_json_number "$BUDGET" "max_hang_count_increase")"
paras_mult="$(extract_json_number "$BUDGET" "max_allocated_paras_multiplier")"

if [[ -z "$latency_mult" || -z "$launch_drop" || -z "$return_drop" || -z "$ready_drop" || -z "$hang_delta" || -z "$paras_mult" ]]; then
  echo "[opengem-perf-check] ERROR: incomplete budget config in $BUDGET" >&2
  exit 1
fi

ACC_ARGS=(--runs "$RUNS" --timeout-sec "$TIMEOUT_SEC" --label "$LABEL")
if [[ "$DO_BUILD" -eq 0 ]]; then
  ACC_ARGS+=(--no-build)
fi
bash scripts/opengem_acceptance_full.sh "${ACC_ARGS[@]}"

ACC_REPORT="build/full/opengem-acceptance-full.${LABEL}.report.txt"
if [[ ! -f "$ACC_REPORT" ]]; then
  echo "[opengem-perf-check] ERROR: missing acceptance report $ACC_REPORT" >&2
  exit 1
fi

cur_launch="$(awk -F': ' '/launch_success_rate_percent/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
cur_return="$(awk -F': ' '/return_to_shell_rate_percent/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
cur_hang="$(awk -F': ' '/hang_count/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
cur_latency="$(awk -F': ' '/average_launch_latency_sec/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"

cur_ready=0
ready_pattern='\[OPENGEM-DESKTOP\][[:space:]]+Starting|OOPPEENNGGEEMM-DDEESSKKTTOOPP.*SSttaarrttiinngg'
ACC_DIR="build/full/opengem-acceptance-${LABEL}"
if compgen -G "$ACC_DIR/run-*.serial.log" >/dev/null; then
  while IFS= read -r f; do
    if grep -Eqi "$ready_pattern" "$f"; then
      cur_ready=$((cur_ready + 1))
    fi
  done < <(ls -1 "$ACC_DIR"/run-*.serial.log)
fi
cur_ready_rate="$(awk -v ok="$cur_ready" -v total="$RUNS" 'BEGIN { printf "%.2f", (ok*100.0)/total }')"

latency_limit="$(awk -v b="$base_latency" -v m="$latency_mult" 'BEGIN { printf "%.3f", b*m }')"
launch_min="$(awk -v b="$base_launch" -v d="$launch_drop" 'BEGIN { v=b-d; if (v<0) v=0; printf "%.2f", v }')"
return_min="$(awk -v b="$base_return" -v d="$return_drop" 'BEGIN { v=b-d; if (v<0) v=0; printf "%.2f", v }')"
ready_min="$(awk -v b="$base_ready" -v d="$ready_drop" 'BEGIN { v=b-d; if (v<0) v=0; printf "%.2f", v }')"
hang_max="$(awk -v b="$base_hang" -v d="$hang_delta" 'BEGIN { printf "%d", int(b+d) }')"

latency_ok="$(awk -v c="$cur_latency" -v l="$latency_limit" 'BEGIN { if (c+0 <= l+0) print 1; else print 0 }')"
launch_ok="$(awk -v c="$cur_launch" -v l="$launch_min" 'BEGIN { if (c+0 >= l+0) print 1; else print 0 }')"
return_ok="$(awk -v c="$cur_return" -v l="$return_min" 'BEGIN { if (c+0 >= l+0) print 1; else print 0 }')"
ready_ok="$(awk -v c="$cur_ready_rate" -v l="$ready_min" 'BEGIN { if (c+0 >= l+0) print 1; else print 0 }')"
hang_ok=0
if [[ "$cur_hang" =~ ^[0-9]+$ ]] && [[ "$cur_hang" -le "$hang_max" ]]; then
  hang_ok=1
fi

cur_paras=0
if compgen -G "$ACC_DIR/run-*.serial.log" >/dev/null; then
  while IFS= read -r f; do
    v="$(strings -a "$f" | awk '
      {
        if (match($0, /M1=[0-9A-F]{4}\/([0-9A-F]{4}) 2=[0-9A-F]{4}\/([0-9A-F]{4})/, m)) {
          v1 = strtonum("0x" m[1]);
          v2 = strtonum("0x" m[2]);
          if (v1 > max) max = v1;
          if (v2 > max) max = v2;
        }
      }
      END { printf "%d", max + 0 }
    ' 2>/dev/null || echo 0)"
    if [[ "$v" =~ ^[0-9]+$ ]] && [[ "$v" -gt "$cur_paras" ]]; then
      cur_paras="$v"
    fi
  done < <(ls -1 "$ACC_DIR"/run-*.serial.log)
fi

paras_limit="$(awk -v b="${base_paras:-0}" -v m="$paras_mult" 'BEGIN { printf "%d", int(b*m + 0.999) }')"
paras_ok=1
if [[ "${base_paras:-0}" -gt 0 ]] && [[ "$cur_paras" -gt "$paras_limit" ]]; then
  paras_ok=0
fi

verdict="PASS"
if [[ "$latency_ok" -ne 1 || "$launch_ok" -ne 1 || "$return_ok" -ne 1 || "$ready_ok" -ne 1 || "$hang_ok" -ne 1 || "$paras_ok" -ne 1 ]]; then
  verdict="FAIL"
fi

REPORT="build/full/opengem-performance-budget-check.${LABEL}.report.txt"
cat > "$REPORT" << EOF
OpenGEM Performance Budget Check (OG-P2-02)
Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Verdict: $verdict

Baseline:
- file: $BASELINE
- launch_success_rate_percent: $base_launch
- return_to_shell_rate_percent: $base_return
- desktop_ready_rate_percent: $base_ready
- hang_count: $base_hang
- average_launch_latency_sec: $base_latency
- observed_max_allocated_paras: ${base_paras:-0}

Budget:
- file: $BUDGET
- max_avg_launch_latency_multiplier: $latency_mult
- max_launch_success_drop_percent: $launch_drop
- max_return_to_shell_drop_percent: $return_drop
- max_desktop_ready_drop_percent: $ready_drop
- max_hang_count_increase: $hang_delta
- max_allocated_paras_multiplier: $paras_mult

Current:
- acceptance_report: $ACC_REPORT
- launch_success_rate_percent: $cur_launch
- return_to_shell_rate_percent: $cur_return
- desktop_ready_rate_percent: $cur_ready_rate
- hang_count: $cur_hang
- average_launch_latency_sec: $cur_latency
- observed_max_allocated_paras: $cur_paras

Computed limits:
- launch_success_min: $launch_min
- return_to_shell_min: $return_min
- desktop_ready_min: $ready_min
- hang_count_max: $hang_max
- avg_launch_latency_max: $latency_limit
- allocated_paras_max: $paras_limit

Checks:
- launch_check: $launch_ok
- return_check: $return_ok
- desktop_ready_check: $ready_ok
- hang_check: $hang_ok
- latency_check: $latency_ok
- allocated_paras_check: $paras_ok
EOF

echo "[opengem-perf-check] report: $REPORT"
if [[ "$verdict" == "PASS" ]]; then
  echo "[opengem-perf-check] PASS"
  exit 0
fi

echo "[opengem-perf-check] FAIL" >&2
exit 1
