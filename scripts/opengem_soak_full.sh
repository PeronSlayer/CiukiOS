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
Usage: scripts/opengem_soak_full.sh [--duration-min <20..30>] [--run-timeout-sec <n>] [--label <name>] [--no-build]

Runs an OpenGEM long-session soak campaign and writes:
  - per-run serial logs in build/full/opengem-soak-<label>/
  - per-run NDJSON metrics in build/full/opengem-soak-full.<label>.runs.ndjson
  - machine-readable JSON report in build/full/opengem-soak-full.<label>.report.json
  - human-readable summary in build/full/opengem-soak-full.<label>.report.txt

Defaults:
  duration-min: 20
  run-timeout-sec: 12
TXT
}

DURATION_MIN="${SOAK_DURATION_MIN:-20}"
RUN_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-12}"
LABEL="latest"
DO_BUILD=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration-min)
      DURATION_MIN="${2:-}"
      [[ -n "$DURATION_MIN" ]] || { echo "[opengem-soak] ERROR: missing --duration-min value" >&2; exit 1; }
      shift 2
      ;;
    --run-timeout-sec)
      RUN_TIMEOUT_SEC="${2:-}"
      [[ -n "$RUN_TIMEOUT_SEC" ]] || { echo "[opengem-soak] ERROR: missing --run-timeout-sec value" >&2; exit 1; }
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      [[ -n "$LABEL" ]] || { echo "[opengem-soak] ERROR: missing --label value" >&2; exit 1; }
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
      echo "[opengem-soak] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "$DURATION_MIN" =~ ^[0-9]+$ ]] || [[ "$DURATION_MIN" -lt 20 ]] || [[ "$DURATION_MIN" -gt 30 ]]; then
  echo "[opengem-soak] ERROR: --duration-min must be an integer in [20,30]" >&2
  exit 1
fi

if ! [[ "$RUN_TIMEOUT_SEC" =~ ^[0-9]+$ ]] || [[ "$RUN_TIMEOUT_SEC" -lt 1 ]]; then
  echo "[opengem-soak] ERROR: --run-timeout-sec must be an integer >= 1" >&2
  exit 1
fi

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[opengem-soak] ERROR: QEMU not found (set QEMU_BIN)." >&2
  exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[opengem-soak] build step"
  bash scripts/build_full.sh
fi

IMG="build/full/ciukios-full.img"
if [[ ! -f "$IMG" ]]; then
  echo "[opengem-soak] ERROR: image not found: $IMG" >&2
  exit 1
fi

OUT_DIR="build/full/opengem-soak-${LABEL}"
RUNS_NDJSON="build/full/opengem-soak-full.${LABEL}.runs.ndjson"
JSON_REPORT="build/full/opengem-soak-full.${LABEL}.report.json"
TXT_REPORT="build/full/opengem-soak-full.${LABEL}.report.txt"
mkdir -p "$OUT_DIR"
rm -f "$RUNS_NDJSON" "$JSON_REPORT" "$TXT_REPORT" "$OUT_DIR"/run-*.serial.log

img_sha_before="$(sha256sum "$IMG" | awk '{print $1}')"
start_epoch="$(date +%s)"
deadline_epoch=$((start_epoch + DURATION_MIN * 60))

run_id=0
launch_success=0
return_success=0
hang_count=0
no_return_count=0
qemu_fail_count=0
infra_fail_count=0
infrastructure_retry_count=0
unexpected_exit_count=0
error_signature_count=0
latency_sum_ms=0

while :; do
  now_epoch="$(date +%s)"
  if [[ "$now_epoch" -ge "$deadline_epoch" ]]; then
    break
  fi

  run_id=$((run_id + 1))
  serial_log="$OUT_DIR/run-${run_id}.serial.log"

  run_start_ms="$(date +%s%3N)"
  set +e
  timeout "$RUN_TIMEOUT_SEC" "$QEMU_CMD" \
    -M pc \
    -cpu pentium3 \
    -m 128 \
    -drive "file=$IMG,format=raw,if=ide" \
    -snapshot \
    -boot c \
    -nographic \
    -chardev "file,id=ser0,path=$serial_log" \
    -serial chardev:ser0 \
    -monitor none \
    -no-reboot \
    -no-shutdown \
    >/dev/null 2>&1
  rc=$?
  set -e
  run_end_ms="$(date +%s%3N)"
  elapsed_ms=$((run_end_ms - run_start_ms))

  launch_ok=0
  return_ok=0
  hang=0
  no_return=0
  qemu_fail=0
  infra_fail=0
  unexpected_exit=0
  sig_count=0

  if [[ -f "$serial_log" ]]; then
    if grep -Eqi '(\[OPENGEM\]|\[\[OOPPEENNGGEEMM\]\]).*(launch|try GEMVDI|llaauunncch|ttrryy  GGEEMMVVDDI)' "$serial_log"; then
      launch_ok=1
    fi
    if grep -Eqi '(\[OPENGEM\]|\[\[OOPPEENNGGEEMM\]\]).*(returned|rreettuurrnneedd|rreettuurrnneed)' "$serial_log"; then
      return_ok=1
    fi
    sig_count="$(grep -Eci '\[IERR\]|\[OPENGEM\] launch failed|\[\[OOPPEENNGGEEMM\]\].*ffaaiilleedd' "$serial_log" || true)"
  fi

  if [[ "$rc" -eq 124 && "$return_ok" -eq 0 ]]; then
    hang=1
  fi

  if [[ "$launch_ok" -eq 1 ]] && [[ "$return_ok" -eq 0 ]]; then
    no_return=1
  fi

  if [[ "$rc" -ne 0 && "$rc" -ne 124 ]]; then
    qemu_fail=1
    if [[ "$launch_ok" -eq 0 ]] && [[ "$return_ok" -eq 0 ]]; then
      infra_fail=1
    elif [[ "$return_ok" -eq 0 ]]; then
      unexpected_exit=1
    fi
  fi

  if [[ "$infra_fail" -eq 1 ]] && [[ "$sig_count" -eq 0 ]]; then
    infrastructure_retry_count=$((infrastructure_retry_count + 1))
    rm -f "$serial_log"
    run_id=$((run_id - 1))
    sleep 1
    continue
  fi

  launch_success=$((launch_success + launch_ok))
  return_success=$((return_success + return_ok))
  hang_count=$((hang_count + hang))
  no_return_count=$((no_return_count + no_return))
  qemu_fail_count=$((qemu_fail_count + qemu_fail))
  infra_fail_count=$((infra_fail_count + infra_fail))
  unexpected_exit_count=$((unexpected_exit_count + unexpected_exit))
  error_signature_count=$((error_signature_count + sig_count))
  latency_sum_ms=$((latency_sum_ms + elapsed_ms))

  printf '{"run":%d,"qemu_rc":%d,"elapsed_ms":%d,"launch_ok":%d,"return_ok":%d,"hang":%d,"launch_without_return":%d,"qemu_fail":%d,"infra_fail":%d,"unexpected_exit":%d,"error_signatures":%d}\n' \
    "$run_id" "$rc" "$elapsed_ms" "$launch_ok" "$return_ok" "$hang" "$no_return" "$qemu_fail" "$infra_fail" "$unexpected_exit" "$sig_count" >> "$RUNS_NDJSON"

  echo "[opengem-soak] run $run_id rc=$rc launch=$launch_ok return=$return_ok hang=$hang noreturn=$no_return qfail=$qemu_fail infra=$infra_fail uexit=$unexpected_exit sig=$sig_count elapsed_ms=$elapsed_ms"
done

end_epoch="$(date +%s)"
actual_duration_sec=$((end_epoch - start_epoch))
img_sha_after="$(sha256sum "$IMG" | awk '{print $1}')"
img_changed=0
if [[ "$img_sha_before" != "$img_sha_after" ]]; then
  img_changed=1
fi

if [[ "$run_id" -eq 0 ]]; then
  echo "[opengem-soak] ERROR: no runs were executed" >&2
  exit 1
fi

launch_rate="$(awk -v ok="$launch_success" -v total="$run_id" 'BEGIN { printf "%.2f", (ok*100.0)/total }')"
return_rate="$(awk -v ok="$return_success" -v total="$run_id" 'BEGIN { printf "%.2f", (ok*100.0)/total }')"
avg_latency_sec="$(awk -v sum_ms="$latency_sum_ms" -v total="$run_id" 'BEGIN { printf "%.3f", (sum_ms/1000.0)/total }')"

cat > "$JSON_REPORT" << EOF
{
  "date_utc": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "profile": "full",
  "label": "$LABEL",
  "duration_target_min": $DURATION_MIN,
  "duration_actual_sec": $actual_duration_sec,
  "run_timeout_sec": $RUN_TIMEOUT_SEC,
  "runs": $run_id,
  "metrics": {
    "launch_success_rate_percent": $launch_rate,
    "return_to_shell_rate_percent": $return_rate,
    "hang_count": $hang_count,
    "launch_without_return_count": $no_return_count,
    "average_launch_latency_sec": $avg_latency_sec,
    "qemu_fail_count": $qemu_fail_count,
    "infra_fail_count": $infra_fail_count,
    "infrastructure_retry_count": $infrastructure_retry_count,
    "unexpected_exit_count": $unexpected_exit_count,
    "error_signature_count": $error_signature_count
  },
  "integrity": {
    "image_sha256_before": "$img_sha_before",
    "image_sha256_after": "$img_sha_after",
    "image_changed": $img_changed
  },
  "artifacts": {
    "runs_ndjson": "$RUNS_NDJSON",
    "runs_dir": "$OUT_DIR"
  }
}
EOF

cat > "$TXT_REPORT" << EOF
OpenGEM full-profile soak report
Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Label: $LABEL

Duration target (min): $DURATION_MIN
Duration actual (sec): $actual_duration_sec
Run timeout (sec): $RUN_TIMEOUT_SEC
Runs executed: $run_id

Metrics:
- launch_success_rate_percent: $launch_rate
- return_to_shell_rate_percent: $return_rate
- hang_count: $hang_count
- launch_without_return_count: $no_return_count
- average_launch_latency_sec: $avg_latency_sec
- qemu_fail_count: $qemu_fail_count
- infra_fail_count: $infra_fail_count
- infrastructure_retry_count: $infrastructure_retry_count
- unexpected_exit_count: $unexpected_exit_count
- error_signature_count: $error_signature_count

Integrity:
- image_sha256_before: $img_sha_before
- image_sha256_after: $img_sha_after
- image_changed: $img_changed

Artifacts:
- $RUNS_NDJSON
- $JSON_REPORT
- $OUT_DIR
EOF

echo "[opengem-soak] report json: $JSON_REPORT"
echo "[opengem-soak] report txt: $TXT_REPORT"
