#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

COSTA_VERSION="${COSTA_VERSION:-1.8.0}"
COSTA_ARCHIVE_SHA256="${COSTA_ARCHIVE_SHA256:-254e79b7617bd96722d228731883ea2aeac982ee22e236d30fa0f9987430ee88}"
COSTA_URL="${COSTA_URL:-https://github.com/jacobpalm/costa/releases/download/v${COSTA_VERSION}/costa180.zip}"
COSTA_DEST_DIR="${CIUKIOS_COSTA_SRC_DIR:-$ROOT_DIR/build/external/costa/v${COSTA_VERSION}}"
COSTA_CACHE_DIR="${CIUKIOS_DOWNLOAD_CACHE_DIR:-$ROOT_DIR/build/downloads}"
COSTA_ARCHIVE="$COSTA_CACHE_DIR/costa-${COSTA_VERSION}.zip"

required_files=(
  COSTA.EXE
  DESKTOP.EXE
  DATA/FONTDATA.BSV
  DATA/FONTINFO.BSV
  LICENSE
)

payload_is_complete() {
  local relative_path
  for relative_path in "${required_files[@]}"; do
    [[ -s "$COSTA_DEST_DIR/$relative_path" ]] || return 1
  done
}

if payload_is_complete; then
  echo "[fetch-costa] ready: $COSTA_DEST_DIR (Costa v$COSTA_VERSION)"
  exit 0
fi

if [[ -e "$COSTA_DEST_DIR" ]]; then
  echo "[fetch-costa] ERROR: incomplete destination already exists: $COSTA_DEST_DIR" >&2
  echo "[fetch-costa] remove that version directory explicitly, then retry" >&2
  exit 1
fi

for command_name in curl sha256sum unzip mktemp; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "[fetch-costa] ERROR: missing required command: $command_name" >&2
    exit 1
  fi
done

mkdir -p "$COSTA_CACHE_DIR" "$(dirname "$COSTA_DEST_DIR")"

archive_ok=0
if [[ -f "$COSTA_ARCHIVE" ]]; then
  actual_hash="$(sha256sum "$COSTA_ARCHIVE" | awk '{print $1}')"
  if [[ "$actual_hash" == "$COSTA_ARCHIVE_SHA256" ]]; then
    archive_ok=1
  else
    echo "[fetch-costa] cached archive hash mismatch; downloading a verified replacement" >&2
  fi
fi

if (( ! archive_ok )); then
  download_tmp="${COSTA_ARCHIVE}.download.$$"
  trap 'rm -f -- "$download_tmp"' EXIT
  echo "[fetch-costa] downloading Costa v$COSTA_VERSION"
  curl --fail --location --retry 3 --retry-delay 1 --output "$download_tmp" "$COSTA_URL"
  actual_hash="$(sha256sum "$download_tmp" | awk '{print $1}')"
  if [[ "$actual_hash" != "$COSTA_ARCHIVE_SHA256" ]]; then
    echo "[fetch-costa] ERROR: SHA-256 mismatch" >&2
    echo "[fetch-costa] expected: $COSTA_ARCHIVE_SHA256" >&2
    echo "[fetch-costa] actual:   $actual_hash" >&2
    exit 1
  fi
  mv "$download_tmp" "$COSTA_ARCHIVE"
  trap - EXIT
fi

extract_tmp="$(mktemp -d "${TMPDIR:-/tmp}/ciukios-costa-fetch.XXXXXX")"
cleanup() {
  rm -rf -- "$extract_tmp"
}
trap cleanup EXIT

unzip -q "$COSTA_ARCHIVE" -d "$extract_tmp"
for relative_path in "${required_files[@]}"; do
  if [[ ! -s "$extract_tmp/$relative_path" ]]; then
    echo "[fetch-costa] ERROR: release is missing $relative_path" >&2
    exit 1
  fi
done

mv "$extract_tmp" "$COSTA_DEST_DIR"
trap - EXIT
echo "[fetch-costa] installed Costa v$COSTA_VERSION: $COSTA_DEST_DIR"
echo "[fetch-costa] archive SHA-256: $COSTA_ARCHIVE_SHA256"
