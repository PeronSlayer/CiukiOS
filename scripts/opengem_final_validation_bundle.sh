#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat << 'TXT'
Usage: scripts/opengem_final_validation_bundle.sh [options]

Aggregates the final OpenGEM validation evidence into a single summary.

Options:
  --label <name>              Output bundle label (default: latest)
  --gate-label <name>         Gate artifact label (default: latest)
  --acceptance-label <name>   Acceptance artifact label (default: gate label)
  --soak-label <name>         Soak artifact label (default: gate label)
  --hardware-dir <path>       Hardware evidence directory (default: docs/hardware)

Outputs:
  - build/full/opengem-final-validation-bundle.<label>.report.txt
  - build/full/opengem-final-validation-bundle.<label>.report.json
TXT
}

OUTPUT_LABEL="latest"
GATE_LABEL="latest"
ACCEPTANCE_LABEL=""
SOAK_LABEL=""
HARDWARE_DIR="docs/hardware"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --label)
      OUTPUT_LABEL="${2:-}"
      [[ -n "$OUTPUT_LABEL" ]] || { echo "[opengem-final-bundle] ERROR: missing value for --label" >&2; exit 1; }
      shift 2
      ;;
    --gate-label)
      GATE_LABEL="${2:-}"
      [[ -n "$GATE_LABEL" ]] || { echo "[opengem-final-bundle] ERROR: missing value for --gate-label" >&2; exit 1; }
      shift 2
      ;;
    --acceptance-label)
      ACCEPTANCE_LABEL="${2:-}"
      [[ -n "$ACCEPTANCE_LABEL" ]] || { echo "[opengem-final-bundle] ERROR: missing value for --acceptance-label" >&2; exit 1; }
      shift 2
      ;;
    --soak-label)
      SOAK_LABEL="${2:-}"
      [[ -n "$SOAK_LABEL" ]] || { echo "[opengem-final-bundle] ERROR: missing value for --soak-label" >&2; exit 1; }
      shift 2
      ;;
    --hardware-dir)
      HARDWARE_DIR="${2:-}"
      [[ -n "$HARDWARE_DIR" ]] || { echo "[opengem-final-bundle] ERROR: missing value for --hardware-dir" >&2; exit 1; }
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[opengem-final-bundle] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$ACCEPTANCE_LABEL" ]]; then
  ACCEPTANCE_LABEL="$GATE_LABEL"
fi

if [[ -z "$SOAK_LABEL" ]]; then
  SOAK_LABEL="$GATE_LABEL"
fi

extract_colon_value() {
  local file="$1"
  local key="$2"
  awk -F': ' -v key="$key" '$1 ~ key {print $2}' "$file" | tail -n 1 | tr -d '[:space:]'
}

json_escape() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

mkdir -p build/full

GATE_REPORT="build/full/opengem-gate-final.${GATE_LABEL}.report.txt"
ACC_REPORT="build/full/opengem-acceptance-full.${ACCEPTANCE_LABEL}.report.txt"
SOAK_REPORT="build/full/opengem-soak-full.${SOAK_LABEL}.report.txt"
OUT_TXT="build/full/opengem-final-validation-bundle.${OUTPUT_LABEL}.report.txt"
OUT_JSON="build/full/opengem-final-validation-bundle.${OUTPUT_LABEL}.report.json"

gate_present=0
acc_present=0
soak_present=0
hardware_dir_present=0

gate_verdict="MISSING"
gate_launch=""
gate_return=""
gate_hang=""

acc_launch=""
acc_return=""
acc_hang=""
acc_latency=""

soak_runs=""
soak_launch=""
soak_return=""
soak_hang=""
soak_latency=""
soak_qemu_fail=""
soak_error_sig=""

if [[ -f "$GATE_REPORT" ]]; then
  gate_present=1
  gate_verdict="$(extract_colon_value "$GATE_REPORT" "Verdict")"
  gate_launch="$(extract_colon_value "$GATE_REPORT" "- launch_success_rate_percent")"
  gate_return="$(extract_colon_value "$GATE_REPORT" "- return_to_shell_rate_percent")"
  gate_hang="$(extract_colon_value "$GATE_REPORT" "- hang_count")"
fi

if [[ -f "$ACC_REPORT" ]]; then
  acc_present=1
  acc_launch="$(extract_colon_value "$ACC_REPORT" "- launch_success_rate_percent")"
  acc_return="$(extract_colon_value "$ACC_REPORT" "- return_to_shell_rate_percent")"
  acc_hang="$(extract_colon_value "$ACC_REPORT" "- hang_count")"
  acc_latency="$(extract_colon_value "$ACC_REPORT" "- average_launch_latency_sec")"
fi

if [[ -f "$SOAK_REPORT" ]]; then
  soak_present=1
  soak_runs="$(extract_colon_value "$SOAK_REPORT" "Runs executed")"
  soak_launch="$(extract_colon_value "$SOAK_REPORT" "- launch_success_rate_percent")"
  soak_return="$(extract_colon_value "$SOAK_REPORT" "- return_to_shell_rate_percent")"
  soak_hang="$(extract_colon_value "$SOAK_REPORT" "- hang_count")"
  soak_latency="$(extract_colon_value "$SOAK_REPORT" "- average_launch_latency_sec")"
  soak_qemu_fail="$(extract_colon_value "$SOAK_REPORT" "- qemu_fail_count")"
  soak_error_sig="$(extract_colon_value "$SOAK_REPORT" "- error_signature_count")"
fi

hardware_execution_count=0
hardware_evidence_count=0
hardware_delta_count=0
hardware_asset_count=0

if [[ -d "$HARDWARE_DIR" ]]; then
  hardware_dir_present=1
  hardware_execution_count=$(find "$HARDWARE_DIR" -maxdepth 1 -type f -name 'opengem-hardware-execution-*.md' | wc -l | tr -d '[:space:]')
  hardware_evidence_count=$(find "$HARDWARE_DIR" -maxdepth 1 -type f -name 'opengem-hardware-evidence-*.json' | wc -l | tr -d '[:space:]')
  hardware_delta_count=$(find "$HARDWARE_DIR" -maxdepth 1 -type f -name 'opengem-hardware-delta-*.md' | wc -l | tr -d '[:space:]')
  hardware_asset_count=$(find "$HARDWARE_DIR" -maxdepth 1 -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.webm' -o -iname '*.mp4' \) | wc -l | tr -d '[:space:]')
fi

bundle_verdict="PASS"
missing_items=()

if [[ "$gate_present" -ne 1 || "$gate_verdict" != "PASS" ]]; then
  bundle_verdict="FAIL"
  missing_items+=("official_gate")
fi

if [[ "$acc_present" -ne 1 ]]; then
  bundle_verdict="FAIL"
  missing_items+=("acceptance_report")
fi

if [[ "$soak_present" -ne 1 ]]; then
  bundle_verdict="FAIL"
  missing_items+=("soak_report")
fi

if [[ "$hardware_dir_present" -ne 1 || "$hardware_execution_count" -lt 1 || "$hardware_evidence_count" -lt 1 || "$hardware_delta_count" -lt 1 ]]; then
  bundle_verdict="FAIL"
  missing_items+=("hardware_evidence")
fi

missing_summary="none"
if [[ ${#missing_items[@]} -gt 0 ]]; then
  missing_summary="$(IFS=,; echo "${missing_items[*]}")"
fi

cat > "$OUT_TXT" << EOF
OpenGEM Final Validation Bundle
Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
BundleLabel: $OUTPUT_LABEL
Verdict: $bundle_verdict

Inputs:
- gate_report: $GATE_REPORT
- acceptance_report: $ACC_REPORT
- soak_report: $SOAK_REPORT
- hardware_dir: $HARDWARE_DIR

Gate:
- present: $gate_present
- verdict: $gate_verdict
- launch_success_rate_percent: ${gate_launch:-N/A}
- return_to_shell_rate_percent: ${gate_return:-N/A}
- hang_count: ${gate_hang:-N/A}

Acceptance:
- present: $acc_present
- launch_success_rate_percent: ${acc_launch:-N/A}
- return_to_shell_rate_percent: ${acc_return:-N/A}
- hang_count: ${acc_hang:-N/A}
- average_launch_latency_sec: ${acc_latency:-N/A}

Soak:
- present: $soak_present
- runs_executed: ${soak_runs:-N/A}
- launch_success_rate_percent: ${soak_launch:-N/A}
- return_to_shell_rate_percent: ${soak_return:-N/A}
- hang_count: ${soak_hang:-N/A}
- average_launch_latency_sec: ${soak_latency:-N/A}
- qemu_fail_count: ${soak_qemu_fail:-N/A}
- error_signature_count: ${soak_error_sig:-N/A}

Hardware evidence:
- dir_present: $hardware_dir_present
- execution_files: $hardware_execution_count
- evidence_files: $hardware_evidence_count
- delta_files: $hardware_delta_count
- media_assets: $hardware_asset_count

Missing or non-green items:
- $missing_summary
EOF

cat > "$OUT_JSON" << EOF
{
  "date_utc": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "bundle_label": "$(json_escape "$OUTPUT_LABEL")",
  "verdict": "$(json_escape "$bundle_verdict")",
  "inputs": {
    "gate_report": "$(json_escape "$GATE_REPORT")",
    "acceptance_report": "$(json_escape "$ACC_REPORT")",
    "soak_report": "$(json_escape "$SOAK_REPORT")",
    "hardware_dir": "$(json_escape "$HARDWARE_DIR")"
  },
  "gate": {
    "present": $gate_present,
    "verdict": "$(json_escape "$gate_verdict")",
    "launch_success_rate_percent": "$(json_escape "${gate_launch:-}")",
    "return_to_shell_rate_percent": "$(json_escape "${gate_return:-}")",
    "hang_count": "$(json_escape "${gate_hang:-}")"
  },
  "acceptance": {
    "present": $acc_present,
    "launch_success_rate_percent": "$(json_escape "${acc_launch:-}")",
    "return_to_shell_rate_percent": "$(json_escape "${acc_return:-}")",
    "hang_count": "$(json_escape "${acc_hang:-}")",
    "average_launch_latency_sec": "$(json_escape "${acc_latency:-}")"
  },
  "soak": {
    "present": $soak_present,
    "runs_executed": "$(json_escape "${soak_runs:-}")",
    "launch_success_rate_percent": "$(json_escape "${soak_launch:-}")",
    "return_to_shell_rate_percent": "$(json_escape "${soak_return:-}")",
    "hang_count": "$(json_escape "${soak_hang:-}")",
    "average_launch_latency_sec": "$(json_escape "${soak_latency:-}")",
    "qemu_fail_count": "$(json_escape "${soak_qemu_fail:-}")",
    "error_signature_count": "$(json_escape "${soak_error_sig:-}")"
  },
  "hardware_evidence": {
    "dir_present": $hardware_dir_present,
    "execution_files": $hardware_execution_count,
    "evidence_files": $hardware_evidence_count,
    "delta_files": $hardware_delta_count,
    "media_assets": $hardware_asset_count
  },
  "missing_or_non_green_items": "$(json_escape "$missing_summary")"
}
EOF

echo "[opengem-final-bundle] text report: $OUT_TXT"
echo "[opengem-final-bundle] json report: $OUT_JSON"
echo "[opengem-final-bundle] verdict: $bundle_verdict"