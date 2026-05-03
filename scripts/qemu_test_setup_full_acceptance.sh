#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD=1
RUN_SMOKE=1

usage() {
  cat << 'TXT'
Usage: scripts/qemu_test_setup_full_acceptance.sh [--no-build] [--skip-smoke]

Checks:
  1) Optional full image build
  2) Optional full smoke boot test (qemu_test_full)
  3) SETUP.COM payload packaged in FAT16 APPS directory
  4) FAT16 chain and payload bytes consistency
TXT
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      DO_BUILD=0
      shift
      ;;
    --skip-smoke)
      RUN_SMOKE=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[setup-accept-full] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

mark_pass() {
  local marker="$1"
  echo "[setup-accept-full] MARKER ${marker}=PASS"
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[setup-accept-full] MARKER ${marker}=FAIL" >&2
  echo "[setup-accept-full] DETAIL ${detail}" >&2
  exit 1
}

read_u8() {
  local file="$1"
  local offset="$2"
  od -An -N1 -j"$offset" -t u1 "$file" | tr -d '[:space:]'
}

read_u16_le() {
  local file="$1"
  local offset="$2"
  od -An -N2 -j"$offset" -t u2 "$file" | tr -d '[:space:]'
}

read_u32_le() {
  local file="$1"
  local offset="$2"
  od -An -N4 -j"$offset" -t u4 "$file" | tr -d '[:space:]'
}

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[setup-accept-full] build step"
  bash scripts/build_full.sh
fi

if [[ "$RUN_SMOKE" -eq 1 ]]; then
  echo "[setup-accept-full] smoke step"
  bash scripts/qemu_test_full.sh
  mark_pass "FULL_SMOKE"
fi

IMG="build/full/ciukios-full.img"
SETUP_BIN="build/full/obj/setup.com"
SETUP_MANIFEST_BIN="build/full/obj/setup.mft"
STAGE1_BIN="build/full/obj/full_stage1.bin"

if [[ ! -f "$IMG" ]]; then
  mark_fail "IMAGE_PRESENT" "missing image: $IMG"
fi
mark_pass "IMAGE_PRESENT"

if [[ ! -f "$SETUP_BIN" ]]; then
  mark_fail "SETUP_BIN_PRESENT" "missing payload: $SETUP_BIN"
fi
mark_pass "SETUP_BIN_PRESENT"

if [[ ! -f "$SETUP_MANIFEST_BIN" ]]; then
  mark_fail "SETUP_MANIFEST_PRESENT" "missing payload: $SETUP_MANIFEST_BIN"
fi
mark_pass "SETUP_MANIFEST_PRESENT"

if [[ ! -f "$STAGE1_BIN" ]]; then
  mark_fail "STAGE1_BIN_PRESENT" "missing stage1 payload: $STAGE1_BIN"
fi
mark_pass "STAGE1_BIN_PRESENT"

SETUP_SIZE="$(stat -c%s "$SETUP_BIN")"
if [[ "$SETUP_SIZE" -le 0 ]]; then
  mark_fail "SETUP_SIZE_VALID" "unexpected size: $SETUP_SIZE"
fi
if [[ "$SETUP_SIZE" -gt 16384 ]]; then
  mark_fail "SETUP_SIZE_VALID" "payload exceeds setup packaging cap (16384 bytes): $SETUP_SIZE"
fi
mark_pass "SETUP_SIZE_VALID"

SETUP_MANIFEST_SIZE="$(stat -c%s "$SETUP_MANIFEST_BIN")"
if [[ "$SETUP_MANIFEST_SIZE" -le 0 ]]; then
  mark_fail "SETUP_MANIFEST_SIZE" "unexpected manifest size: $SETUP_MANIFEST_SIZE"
fi
if [[ "$SETUP_MANIFEST_SIZE" -gt 4096 ]]; then
  mark_fail "SETUP_MANIFEST_SIZE" "manifest exceeds single-cluster contract: $SETUP_MANIFEST_SIZE"
fi
mark_pass "SETUP_MANIFEST_SIZE"

STAGE1_SIZE="$(stat -c%s "$STAGE1_BIN")"
STAGE1_SLOT_SIZE=$((61 * 512))
if (( STAGE1_SIZE > STAGE1_SLOT_SIZE )); then
  mark_fail "STAGE1_SLOT_FIT" "stage1 size=$STAGE1_SIZE slot=$STAGE1_SLOT_SIZE"
fi
mark_pass "STAGE1_SLOT_FIT"

echo "[setup-accept-full] MARKER STAGE1_SIZE=${STAGE1_SIZE}"
echo "[setup-accept-full] MARKER STAGE1_SLOT=${STAGE1_SLOT_SIZE}"

FAT_SECTORS_PER_CLUSTER=8
STAGE1_SECTORS=61
FAT_RESERVED_SECTORS=$((1 + STAGE1_SECTORS))
FAT_SECTORS_PER_FAT=128
FAT_COUNT=2
ROOT_ENTRIES=512
ROOT_DIR_SECTORS=$((ROOT_ENTRIES * 32 / 512))
FAT1_LBA=$FAT_RESERVED_SECTORS
ROOT_LBA=$((FAT1_LBA + FAT_SECTORS_PER_FAT * FAT_COUNT))
DATA_LBA=$((ROOT_LBA + ROOT_DIR_SECTORS))
ROOT_APPS_CLUSTER=3
CLUSTER_BYTES=$((FAT_SECTORS_PER_CLUSTER * 512))
SETUP_CLUSTERS=$(((SETUP_SIZE + CLUSTER_BYTES - 1) / CLUSTER_BYTES))

APPS_DIR_OFFSET=$(((DATA_LBA + ((ROOT_APPS_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER)) * 512))
SETUP_ENTRY_OFFSET=$((APPS_DIR_OFFSET + 288))
MANIFEST_ENTRY_OFFSET=$((APPS_DIR_OFFSET + 320))

ENTRY_NAME="$(dd if="$IMG" bs=1 skip="$SETUP_ENTRY_OFFSET" count=11 status=none)"
if [[ "$ENTRY_NAME" != "SETUP   COM" ]]; then
  mark_fail "SETUP_DIR_NAME" "entry mismatch at APPS offset 288 (got '$ENTRY_NAME')"
fi
mark_pass "SETUP_DIR_NAME"

ENTRY_ATTR="$(read_u8 "$IMG" $((SETUP_ENTRY_OFFSET + 11)))"
if [[ "$ENTRY_ATTR" != "32" ]]; then
  mark_fail "SETUP_DIR_ATTR" "expected attr=32 got $ENTRY_ATTR"
fi
mark_pass "SETUP_DIR_ATTR"

ENTRY_CLUSTER="$(read_u16_le "$IMG" $((SETUP_ENTRY_OFFSET + 26)))"
if [[ -z "$ENTRY_CLUSTER" || "$ENTRY_CLUSTER" -le 1 ]]; then
  mark_fail "SETUP_DIR_CLUSTER" "invalid start cluster: $ENTRY_CLUSTER"
fi
mark_pass "SETUP_DIR_CLUSTER"

ENTRY_SIZE="$(read_u32_le "$IMG" $((SETUP_ENTRY_OFFSET + 28)))"
if [[ "$ENTRY_SIZE" != "$SETUP_SIZE" ]]; then
  mark_fail "SETUP_DIR_SIZE" "entry size=$ENTRY_SIZE payload size=$SETUP_SIZE"
fi
mark_pass "SETUP_DIR_SIZE"

FAT_ENTRY_OFFSET=$(((FAT1_LBA * 512) + (ENTRY_CLUSTER * 2)))
FAT_ENTRY_VALUE="$(read_u16_le "$IMG" "$FAT_ENTRY_OFFSET")"

SETUP_CHAIN_CLUSTER="$ENTRY_CLUSTER"
SETUP_CHAIN_REMAINING="$SETUP_CLUSTERS"
while (( SETUP_CHAIN_REMAINING > 1 )); do
  FAT_ENTRY_OFFSET=$(((FAT1_LBA * 512) + (SETUP_CHAIN_CLUSTER * 2)))
  FAT_ENTRY_VALUE="$(read_u16_le "$IMG" "$FAT_ENTRY_OFFSET")"
  EXPECTED_NEXT=$((SETUP_CHAIN_CLUSTER + 1))
  if (( FAT_ENTRY_VALUE != EXPECTED_NEXT )); then
    mark_fail "SETUP_FAT_CHAIN" "expected next cluster $EXPECTED_NEXT got $FAT_ENTRY_VALUE"
  fi
  SETUP_CHAIN_CLUSTER="$FAT_ENTRY_VALUE"
  SETUP_CHAIN_REMAINING=$((SETUP_CHAIN_REMAINING - 1))
done

FAT_ENTRY_OFFSET=$(((FAT1_LBA * 512) + (SETUP_CHAIN_CLUSTER * 2)))
FAT_ENTRY_VALUE="$(read_u16_le "$IMG" "$FAT_ENTRY_OFFSET")"
if [[ "$FAT_ENTRY_VALUE" != "65535" ]]; then
  mark_fail "SETUP_FAT_CHAIN" "expected EOC=65535 got $FAT_ENTRY_VALUE"
fi
mark_pass "SETUP_FAT_CHAIN"

SETUP_DATA_OFFSET=$(((DATA_LBA + ((ENTRY_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER)) * 512))
if ! cmp -n "$SETUP_SIZE" "$SETUP_BIN" <(dd if="$IMG" bs=1 skip="$SETUP_DATA_OFFSET" count="$SETUP_SIZE" status=none) >/dev/null 2>&1; then
  mark_fail "SETUP_PAYLOAD_MATCH" "image payload differs from $SETUP_BIN"
fi
mark_pass "SETUP_PAYLOAD_MATCH"

MANIFEST_ENTRY_NAME="$(dd if="$IMG" bs=1 skip="$MANIFEST_ENTRY_OFFSET" count=11 status=none)"
if [[ "$MANIFEST_ENTRY_NAME" != "SETUPMFTBIN" ]]; then
  mark_fail "SETUP_MFT_DIR_NAME" "entry mismatch at APPS offset 320 (got '$MANIFEST_ENTRY_NAME')"
fi
mark_pass "SETUP_MFT_DIR_NAME"

MANIFEST_ENTRY_ATTR="$(read_u8 "$IMG" $((MANIFEST_ENTRY_OFFSET + 11)))"
if [[ "$MANIFEST_ENTRY_ATTR" != "32" ]]; then
  mark_fail "SETUP_MFT_DIR_ATTR" "expected attr=32 got $MANIFEST_ENTRY_ATTR"
fi
mark_pass "SETUP_MFT_DIR_ATTR"

MANIFEST_ENTRY_CLUSTER="$(read_u16_le "$IMG" $((MANIFEST_ENTRY_OFFSET + 26)))"
if [[ -z "$MANIFEST_ENTRY_CLUSTER" || "$MANIFEST_ENTRY_CLUSTER" -le 1 ]]; then
  mark_fail "SETUP_MFT_DIR_CLUSTER" "invalid start cluster: $MANIFEST_ENTRY_CLUSTER"
fi
mark_pass "SETUP_MFT_DIR_CLUSTER"

MANIFEST_ENTRY_SIZE="$(read_u32_le "$IMG" $((MANIFEST_ENTRY_OFFSET + 28)))"
if [[ "$MANIFEST_ENTRY_SIZE" != "$SETUP_MANIFEST_SIZE" ]]; then
  mark_fail "SETUP_MFT_DIR_SIZE" "entry size=$MANIFEST_ENTRY_SIZE payload size=$SETUP_MANIFEST_SIZE"
fi
mark_pass "SETUP_MFT_DIR_SIZE"

MANIFEST_FAT_ENTRY_OFFSET=$(((FAT1_LBA * 512) + (MANIFEST_ENTRY_CLUSTER * 2)))
MANIFEST_FAT_ENTRY_VALUE="$(read_u16_le "$IMG" "$MANIFEST_FAT_ENTRY_OFFSET")"
if [[ "$MANIFEST_FAT_ENTRY_VALUE" != "65535" ]]; then
  mark_fail "SETUP_MFT_FAT_CHAIN" "expected EOC=65535 got $MANIFEST_FAT_ENTRY_VALUE"
fi
mark_pass "SETUP_MFT_FAT_CHAIN"

MANIFEST_DATA_OFFSET=$(((DATA_LBA + ((MANIFEST_ENTRY_CLUSTER - 2) * FAT_SECTORS_PER_CLUSTER)) * 512))
if ! cmp -n "$SETUP_MANIFEST_SIZE" "$SETUP_MANIFEST_BIN" <(dd if="$IMG" bs=1 skip="$MANIFEST_DATA_OFFSET" count="$SETUP_MANIFEST_SIZE" status=none) >/dev/null 2>&1; then
  mark_fail "SETUP_MFT_PAYLOAD_MATCH" "image payload differs from $SETUP_MANIFEST_BIN"
fi
mark_pass "SETUP_MFT_PAYLOAD_MATCH"

echo "[setup-accept-full] PASS (FULL-only setup packaging acceptance)"
