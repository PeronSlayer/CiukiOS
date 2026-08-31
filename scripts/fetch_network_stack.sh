#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

NETWORK_VERSION="${CIUKIOS_NETWORK_VERSION:-mtcp-2025-01-10_crynwr-2006-09-02c}"
NETWORK_DEST_DIR="${CIUKIOS_NETWORK_SRC_DIR:-$ROOT_DIR/build/external/network/$NETWORK_VERSION}"
NETWORK_CACHE_DIR="${CIUKIOS_DOWNLOAD_CACHE_DIR:-$ROOT_DIR/build/downloads}"

MTCP_URL="${CIUKIOS_MTCP_URL:-https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/mtcp.zip}"
MTCP_SHA256="${CIUKIOS_MTCP_SHA256:-087ae50048004a8fd23f26f3e3a1bac7985107d1912e9244cf5ec0488432aedc}"
MTCP_ARCHIVE="$NETWORK_CACHE_DIR/mtcp-2025-01-10-freedos.zip"

CRYNWR_URL="${CIUKIOS_CRYNWR_URL:-https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/crynwr.zip}"
CRYNWR_SHA256="${CIUKIOS_CRYNWR_SHA256:-b5761639a1bf4ad4fa93bfa4643ca62ae46d08412c78d45ae1b5e10d91d0b9c8}"
CRYNWR_ARCHIVE="$NETWORK_CACHE_DIR/crynwr-2006-09-02c-freedos.zip"

required_files=(
  MTCP/dhcp.exe
  MTCP/ftp.exe
  MTCP/ftpsrv.exe
  MTCP/ping.exe
  MTCP/pkttool.exe
  MTCP/COPYING.TXT
  MTCP/SOURCES.ZIP
  PACKET/NE2000.COM
  PACKET/PKTCHK.COM
  PACKET/GPL.DOC
  PACKET/SOURCES.ZIP
)

payload_is_complete() {
  local relative_path
  for relative_path in "${required_files[@]}"; do
    [[ -s "$NETWORK_DEST_DIR/$relative_path" ]] || return 1
  done
}

download_verified() {
  local label="$1"
  local url="$2"
  local expected_hash="$3"
  local archive="$4"
  local actual_hash
  local download_tmp

  if [[ -f "$archive" ]]; then
    actual_hash="$(sha256sum "$archive" | awk '{print $1}')"
    if [[ "$actual_hash" == "$expected_hash" ]]; then
      return 0
    fi
    echo "[fetch-network] cached $label archive hash mismatch; downloading a verified replacement" >&2
  fi

  download_tmp="${archive}.download.$$"
  trap 'rm -f -- "$download_tmp"' RETURN
  echo "[fetch-network] downloading $label"
  curl --fail --location --retry 3 --retry-delay 1 --output "$download_tmp" "$url"
  actual_hash="$(sha256sum "$download_tmp" | awk '{print $1}')"
  if [[ "$actual_hash" != "$expected_hash" ]]; then
    echo "[fetch-network] ERROR: $label SHA-256 mismatch" >&2
    echo "[fetch-network] expected: $expected_hash" >&2
    echo "[fetch-network] actual:   $actual_hash" >&2
    rm -f -- "$download_tmp"
    exit 1
  fi
  mv "$download_tmp" "$archive"
  trap - RETURN
}

if payload_is_complete; then
  echo "[fetch-network] ready: $NETWORK_DEST_DIR"
  exit 0
fi

if [[ -e "$NETWORK_DEST_DIR" ]]; then
  echo "[fetch-network] ERROR: incomplete destination already exists: $NETWORK_DEST_DIR" >&2
  echo "[fetch-network] remove that version directory explicitly, then retry" >&2
  exit 1
fi

for command_name in curl sha256sum unzip mktemp; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "[fetch-network] ERROR: missing required command: $command_name" >&2
    exit 1
  fi
done

mkdir -p "$NETWORK_CACHE_DIR" "$(dirname "$NETWORK_DEST_DIR")"
download_verified "mTCP 2025-01-10" "$MTCP_URL" "$MTCP_SHA256" "$MTCP_ARCHIVE"
download_verified "Crynwr packet drivers 2006-09-02c" "$CRYNWR_URL" "$CRYNWR_SHA256" "$CRYNWR_ARCHIVE"

extract_tmp="$(mktemp -d "${TMPDIR:-/tmp}/ciukios-network-fetch.XXXXXX")"
cleanup() {
  rm -rf -- "$extract_tmp"
}
trap cleanup EXIT

mkdir -p "$extract_tmp/MTCP" "$extract_tmp/PACKET"
unzip -j -q "$MTCP_ARCHIVE" \
  'NET/mTCP/*.exe' \
  'NET/mTCP/COPYING.TXT' \
  'SOURCE/mTCP/SOURCES.ZIP' \
  -d "$extract_tmp/MTCP"
unzip -j -q "$CRYNWR_ARCHIVE" \
  'DRIVERS/CRYNWR/NE2000.COM' \
  'DRIVERS/CRYNWR/PKTCHK.COM' \
  'DOC/CRYNWR/GPL.DOC' \
  'SOURCE/CRYNWR/SOURCES.ZIP' \
  -d "$extract_tmp/PACKET"

for relative_path in "${required_files[@]}"; do
  if [[ ! -s "$extract_tmp/$relative_path" ]]; then
    echo "[fetch-network] ERROR: extracted payload is missing $relative_path" >&2
    exit 1
  fi
done

mv "$extract_tmp" "$NETWORK_DEST_DIR"
trap - EXIT
echo "[fetch-network] installed: $NETWORK_DEST_DIR"
echo "[fetch-network] mTCP SHA-256:  $MTCP_SHA256"
echo "[fetch-network] Crynwr SHA-256: $CRYNWR_SHA256"
