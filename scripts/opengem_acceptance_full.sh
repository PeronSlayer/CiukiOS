#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return
  fi
  if command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "qemu-system-i386"
    return
  fi
  if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64"
    return
  fi
  return 1
}

usage() {
  cat << 'TXT'
Usage: scripts/opengem_acceptance_full.sh [--runs <n>] [--timeout-sec <n>] [--no-build] [--label <name>]

Runs OpenGEM full-profile acceptance campaign and writes:
  - per-run serial logs in build/full/opengem-acceptance-<label>/
  - final report in build/full/opengem-acceptance-full.<label>.report.txt

Metrics:
  - launch success rate
  - return-to-shell rate
  - hang count
  - average launch latency (seconds)
TXT
}

RUNS="${RUNS:-20}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-30}"
DO_BUILD=1
LABEL="latest"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)
      RUNS="${2:-}"
      if [[ -z "$RUNS" ]]; then
        echo "[opengem-acceptance] ERROR: missing value for --runs" >&2
        exit 1
      fi
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC="${2:-}"
      if [[ -z "$TIMEOUT_SEC" ]]; then
        echo "[opengem-acceptance] ERROR: missing value for --timeout-sec" >&2
        exit 1
      fi
      shift 2
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --label)
      LABEL="${2:-}"
      if [[ -z "$LABEL" ]]; then
        echo "[opengem-acceptance] ERROR: missing value for --label" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[opengem-acceptance] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "$RUNS" =~ ^[0-9]+$ ]] || [[ "$RUNS" -lt 1 ]]; then
  echo "[opengem-acceptance] ERROR: --runs must be an integer >= 1" >&2
  exit 1
fi

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$TIMEOUT_SEC" -lt 1 ]]; then
  echo "[opengem-acceptance] ERROR: --timeout-sec must be an integer >= 1" >&2
  exit 1
fi

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[opengem-acceptance] ERROR: QEMU not found (set QEMU_BIN)." >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[opengem-acceptance] build step"
  bash scripts/build_full.sh
fi

IMG="build/full/ciukios-full.img"
if [[ ! -f "$IMG" ]]; then
  echo "[opengem-acceptance] ERROR: image not found: $IMG" >&2
  exit 1
fi

OUT_DIR="build/full/opengem-acceptance-${LABEL}"
REPORT="build/full/opengem-acceptance-full.${LABEL}.report.txt"
mkdir -p "$OUT_DIR"
rm -f "$REPORT" "$OUT_DIR"/run-*.serial.log "$OUT_DIR"/run-*.meta.txt

launch_success=0
return_success=0
hang_count=0
no_return_count=0
qemu_fail_count=0
infra_fail_count=0
unexpected_exit_count=0
latency_sum_ms=0

run_one() {
  local idx="$1"
  local serial_log="$OUT_DIR/run-${idx}.serial.log"
  local meta_log="$OUT_DIR/run-${idx}.meta.txt"

  local start_ms
  local end_ms
  local elapsed_ms
  start_ms="$(date +%s%3N)"

  timeout "$TIMEOUT_SEC" "$QEMU_CMD" \
    -M pc \
    -cpu pentium3 \
    -m 128 \
    -drive "file=$IMG,format=raw,if=ide" \
    -boot c \
    -nographic \
    -chardev "file,id=ser0,path=$serial_log" \
    -serial chardev:ser0 \
    -monitor none \
    -no-reboot \
    -no-shutdown \
    >/dev/null 2>&1
  local rc=$?

  end_ms="$(date +%s%3N)"
  elapsed_ms=$((end_ms - start_ms))

  local launch_ok=0
  local return_ok=0
  local hang=0
  local no_return=0
  local qemu_fail=0
  local infra_fail=0
  local unexpected_exit=0

  if grep -Eqi '(\[OPENGEM\]|\[\[OOPPEENNGGEEMM\]\]).*(launch|try GEMVDI|llaauunncch|ttrryy  GGEEMMVVDDI)' "$serial_log"; then
    launch_ok=1
  fi

  if grep -Eqi '(\[OPENGEM\]|\[\[OOPPEENNGGEEMM\]\]).*(returned|rreettuurrnneedd|rreettuurrnneed)' "$serial_log"; then
    return_ok=1
  fi

  if [[ $rc -eq 124 ]] && [[ "$return_ok" -eq 0 ]]; then
    hang=1
  fi

  if [[ "$launch_ok" -eq 1 ]] && [[ "$return_ok" -eq 0 ]]; then
    no_return=1
  fi

  if [[ $rc -ne 0 ]] && [[ $rc -ne 124 ]]; then
    qemu_fail=1
    if [[ "$launch_ok" -eq 0 ]] && [[ "$return_ok" -eq 0 ]]; then
      infra_fail=1
    elif [[ "$return_ok" -eq 0 ]]; then
      unexpected_exit=1
    fi
  fi

  {
    echo "run=$idx"
    echo "qemu_rc=$rc"
    echo "elapsed_ms=$elapsed_ms"
    echo "launch_ok=$launch_ok"
    echo "return_ok=$return_ok"
    echo "hang=$hang"
    echo "no_return=$no_return"
    echo "qemu_fail=$qemu_fail"
    echo "infra_fail=$infra_fail"
    echo "unexpected_exit=$unexpected_exit"
  } > "$meta_log"

  echo "$launch_ok $return_ok $hang $no_return $qemu_fail $infra_fail $unexpected_exit $elapsed_ms $rc"
}

echo "[opengem-acceptance] running ${RUNS} iterations (timeout=${TIMEOUT_SEC}s)"

for i in $(seq 1 "$RUNS"); do
  set +e
  line="$(run_one "$i")"
  run_rc=$?
  set -e

  if [[ $run_rc -ne 0 ]]; then
    echo "[opengem-acceptance] run $i failed unexpectedly (internal harness error)" >&2
    exit 1
  fi

  read -r launch_ok return_ok hang no_return qemu_fail infra_fail unexpected_exit elapsed_ms qemu_rc <<< "$line"

  launch_success=$((launch_success + launch_ok))
  return_success=$((return_success + return_ok))
  hang_count=$((hang_count + hang))
  no_return_count=$((no_return_count + no_return))
  qemu_fail_count=$((qemu_fail_count + qemu_fail))
  infra_fail_count=$((infra_fail_count + infra_fail))
  unexpected_exit_count=$((unexpected_exit_count + unexpected_exit))
  latency_sum_ms=$((latency_sum_ms + elapsed_ms))

  echo "[opengem-acceptance] run $i/$RUNS rc=$qemu_rc launch=$launch_ok return=$return_ok hang=$hang noreturn=$no_return qfail=$qemu_fail infra=$infra_fail uexit=$unexpected_exit elapsed_ms=$elapsed_ms"
done

launch_rate="$(awk -v ok="$launch_success" -v total="$RUNS" 'BEGIN { printf "%.2f", (ok*100.0)/total }')"
return_rate="$(awk -v ok="$return_success" -v total="$RUNS" 'BEGIN { printf "%.2f", (ok*100.0)/total }')"
avg_latency_sec="$(awk -v sum_ms="$latency_sum_ms" -v total="$RUNS" 'BEGIN { printf "%.3f", (sum_ms/1000.0)/total }')"

cat > "$REPORT" << EOF
OpenGEM full-profile acceptance report
Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Runs: $RUNS
TimeoutSec: $TIMEOUT_SEC

Artifacts directory: $OUT_DIR

Metrics:
- launch_success_rate_percent: $launch_rate
- return_to_shell_rate_percent: $return_rate
- hang_count: $hang_count
- launch_without_return_count: $no_return_count
- qemu_fail_count: $qemu_fail_count
- infra_fail_count: $infra_fail_count
- unexpected_exit_count: $unexpected_exit_count
- average_launch_latency_sec: $avg_latency_sec

Raw counts:
- launch_success: $launch_success/$RUNS
- return_to_shell_success: $return_success/$RUNS
- hangs: $hang_count/$RUNS
- launch_without_return: $no_return_count/$RUNS
- qemu_failures: $qemu_fail_count/$RUNS
- infra_failures: $infra_fail_count/$RUNS
- unexpected_exits: $unexpected_exit_count/$RUNS
EOF

echo "[opengem-acceptance] done"
echo "[opengem-acceptance] report: $REPORT"
