#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat << 'TXT'
Usage: scripts/opengem_perf_baseline.sh [--runs <n>] [--timeout-sec <n>] [--label <name>] [--no-build]

Generates OG-P2-02 baseline metrics from OpenGEM acceptance runs:
- launch success rate
- return-to-shell rate
- hang count
- average launch latency
- observed DOS allocator high-water mark (paras) from serial traces

Outputs:
- build/full/opengem-performance-baseline.<label>.json
TXT
}

RUNS="${RUNS:-20}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-30}"
LABEL="latest"
DO_BUILD=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)
      RUNS="${2:-}"
      [[ -n "$RUNS" ]] || { echo "[opengem-perf-baseline] ERROR: missing value for --runs" >&2; exit 1; }
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC="${2:-}"
      [[ -n "$TIMEOUT_SEC" ]] || { echo "[opengem-perf-baseline] ERROR: missing value for --timeout-sec" >&2; exit 1; }
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      [[ -n "$LABEL" ]] || { echo "[opengem-perf-baseline] ERROR: missing value for --label" >&2; exit 1; }
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
      echo "[opengem-perf-baseline] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "$RUNS" =~ ^[0-9]+$ ]] || [[ "$RUNS" -lt 1 ]]; then
  echo "[opengem-perf-baseline] ERROR: --runs must be integer >= 1" >&2
  exit 1
fi

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$TIMEOUT_SEC" -lt 1 ]]; then
  echo "[opengem-perf-baseline] ERROR: --timeout-sec must be integer >= 1" >&2
  exit 1
fi

ACC_ARGS=(--runs "$RUNS" --timeout-sec "$TIMEOUT_SEC" --label "$LABEL")
if [[ "$DO_BUILD" -eq 0 ]]; then
  ACC_ARGS+=(--no-build)
fi

echo "[opengem-perf-baseline] running acceptance capture"
bash scripts/opengem_acceptance_full.sh "${ACC_ARGS[@]}"

ACC_REPORT="build/full/opengem-acceptance-full.${LABEL}.report.txt"
ACC_DIR="build/full/opengem-acceptance-${LABEL}"
OUT_JSON="build/full/opengem-performance-baseline.${LABEL}.json"

if [[ ! -f "$ACC_REPORT" ]]; then
  echo "[opengem-perf-baseline] ERROR: missing acceptance report $ACC_REPORT" >&2
  exit 1
fi

launch_rate="$(awk -F': ' '/launch_success_rate_percent/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
return_rate="$(awk -F': ' '/return_to_shell_rate_percent/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
hang_count="$(awk -F': ' '/hang_count/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"
avg_latency="$(awk -F': ' '/average_launch_latency_sec/ {print $2}' "$ACC_REPORT" | tail -n 1 | tr -d '[:space:]')"

if [[ -z "$launch_rate" || -z "$return_rate" || -z "$hang_count" || -z "$avg_latency" ]]; then
  echo "[opengem-perf-baseline] ERROR: incomplete metrics in $ACC_REPORT" >&2
  exit 1
fi

max_paras=0
if compgen -G "$ACC_DIR/run-*.serial.log" >/dev/null; then
  while IFS= read -r f; do
    cur="$(strings -a "$f" | awk '
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
    if [[ "$cur" =~ ^[0-9]+$ ]] && [[ "$cur" -gt "$max_paras" ]]; then
      max_paras="$cur"
    fi
  done < <(ls -1 "$ACC_DIR"/run-*.serial.log)
fi

cat > "$OUT_JSON" << EOF
{
  "date_utc": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "label": "$LABEL",
  "runs": $RUNS,
  "timeout_sec": $TIMEOUT_SEC,
  "launch_success_rate_percent": $launch_rate,
  "return_to_shell_rate_percent": $return_rate,
  "hang_count": $hang_count,
  "average_launch_latency_sec": $avg_latency,
  "observed_max_allocated_paras": $max_paras,
  "source_acceptance_report": "$ACC_REPORT",
  "source_acceptance_dir": "$ACC_DIR"
}
EOF

echo "[opengem-perf-baseline] baseline: $OUT_JSON"