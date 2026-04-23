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
Usage: scripts/opengem_trace_full.sh [--no-build] [--timeout-sec <n>] [--label <name>]

Generates OpenGEM full-profile tracing artifacts under build/full/:
  - opengem-trace-full.<label>.serial.log
  - opengem-trace-full.<label>.qemu-int.log
  - opengem-trace-full.<label>.int21-summary.txt

Options:
  --no-build         Skip full image rebuild.
  --timeout-sec <n>  QEMU timeout in seconds (default: 30).
  --label <name>     Suffix label for output files (default: latest).
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
      if [[ -z "$TIMEOUT_SEC" ]]; then
        echo "[opengem-trace] ERROR: missing value for --timeout-sec" >&2
        exit 1
      fi
      shift 2
      ;;
    --label)
      LABEL="${2:-}"
      if [[ -z "$LABEL" ]]; then
        echo "[opengem-trace] ERROR: missing value for --label" >&2
        exit 1
      fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[opengem-trace] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]]; then
  echo "[opengem-trace] ERROR: timeout must be an integer (seconds)" >&2
  exit 1
fi

if ! QEMU_CMD="$(pick_qemu)"; then
  echo "[opengem-trace] ERROR: QEMU not found (set QEMU_BIN)." >&2
  exit 1
fi

mkdir -p build/full

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[opengem-trace] build step"
  bash scripts/build_full.sh
fi

IMG="build/full/ciukios-full.img"
if [[ ! -f "$IMG" ]]; then
  echo "[opengem-trace] ERROR: image not found: $IMG" >&2
  exit 1
fi

SERIAL_LOG="build/full/opengem-trace-full.${LABEL}.serial.log"
QEMU_INT_LOG="build/full/opengem-trace-full.${LABEL}.qemu-int.log"
SUMMARY_LOG="build/full/opengem-trace-full.${LABEL}.int21-summary.txt"

rm -f "$SERIAL_LOG" "$QEMU_INT_LOG" "$SUMMARY_LOG"

QEMU_ARGS=(
  -M pc
  -cpu pentium3
  -m 128
  -drive "file=$IMG,format=raw,if=ide"
  -boot c
  -nographic
  -chardev "file,id=ser0,path=$SERIAL_LOG"
  -serial chardev:ser0
  -monitor none
  -no-reboot
  -no-shutdown
  -d int
  -D "$QEMU_INT_LOG"
)

if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  EXTRA_ARGS=(${QEMU_EXTRA_ARGS})
  QEMU_ARGS+=("${EXTRA_ARGS[@]}")
fi

echo "[opengem-trace] running QEMU trace (timeout=${TIMEOUT_SEC}s)"
set +e
timeout "$TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" >/dev/null 2>&1
RC=$?
set -e

if [[ $RC -ne 0 && $RC -ne 124 ]]; then
  echo "[opengem-trace] FAIL: qemu exited with code $RC" >&2
  exit "$RC"
fi

INT21_COUNT=0
if [[ -s "$QEMU_INT_LOG" ]]; then
  INT21_COUNT="$(grep -Eci 'int[^[:alnum:]]*0x?21|v=21' "$QEMU_INT_LOG" || true)"
fi

AH_PATTERN_COUNT=0
if [[ -s "$SERIAL_LOG" ]]; then
  AH_PATTERN_COUNT="$(grep -Eic '(^|[^0-9A-Fa-f])(06|09|0B|0C|0E|1A|2A|2C|2F|3B|3C|3D|3E|3F|40|41|42|43|44|48|49|4A|4B|4D|4E|4F|51|56|62)(:|!|=)' "$SERIAL_LOG" || true)"
fi

LAUNCH_MARKERS="$(grep -Eci '(\[OPENGEM\]|\[\[OOPPEENNGGEEMM\]\]).*(launch|try GEMVDI|returned|launch failed|llaunch|ttrryy)' "$SERIAL_LOG" || true)"

cat > "$SUMMARY_LOG" << EOF
OpenGEM DOS syscall trace summary
Date: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
Profile: full
TimeoutSec: $TIMEOUT_SEC
QemuExitCode: $RC

Artifacts:
- Serial log: $SERIAL_LOG
- QEMU interrupt log: $QEMU_INT_LOG

Observed counters:
- QEMU INT21-like entries (-d int): $INT21_COUNT
- Serial AH-pattern entries: $AH_PATTERN_COUNT
- OpenGEM launch markers: $LAUNCH_MARKERS

Primary serial snippets:
EOF

if [[ -s "$SERIAL_LOG" ]]; then
  grep -E '\[BOOT0-FULL\]|\[STAGE1-SERIAL\]|\[OPENGEM\]|\[\[OOPPEENNGGEEMM\]\]|(^|[^0-9A-Fa-f])(3B|3E|48|4B|4D|4E)(:|!|=)' "$SERIAL_LOG" | head -n 80 >> "$SUMMARY_LOG" || true
else
  echo "(serial log empty)" >> "$SUMMARY_LOG"
fi

echo "[opengem-trace] done"
echo "[opengem-trace] serial: $SERIAL_LOG"
echo "[opengem-trace] qemu-int: $QEMU_INT_LOG"
echo "[opengem-trace] summary: $SUMMARY_LOG"
