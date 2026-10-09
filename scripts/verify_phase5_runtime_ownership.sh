#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<'TXT'
Usage: scripts/verify_phase5_runtime_ownership.sh [--no-build]

Verifies the Phase 5 loader/kernel ownership boundary and the versioned
CIUKIDOS ABI image.  By default the full image is rebuilt first.
TXT
}

DO_BUILD=1
case "${1:-}" in
  "") ;;
  --no-build) DO_BUILD=0 ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    echo "[phase5-ownership] ERROR: unknown argument: $1" >&2
    usage >&2
    exit 2
    ;;
esac
if [[ $# -gt 1 ]]; then
  echo "[phase5-ownership] ERROR: too many arguments" >&2
  usage >&2
  exit 2
fi

fail() {
  echo "[phase5-ownership] FAIL: $1" >&2
  exit 1
}

read_u16_le() {
  local file="$1"
  local offset="$2"
  local lo hi
  read -r lo hi < <(od -An -tu1 -j "$offset" -N 2 "$file")
  [[ -n "${lo:-}" && -n "${hi:-}" ]] || fail "cannot read u16 at offset $offset from $file"
  printf '%u\n' "$((lo + (hi << 8)))"
}

read_hex() {
  local file="$1"
  local offset="$2"
  local count="$3"
  od -An -tx1 -j "$offset" -N "$count" "$file" | tr -d ' \n'
}

if (( DO_BUILD )); then
  echo "[phase5-ownership] rebuilding the canonical full profile"
  bash scripts/build_full.sh
fi

LOADER_SRC="src/boot/full_stage1_loader.asm"
KERNEL_SRC="src/runtime/ciukidos.asm"
SERVICES_SRC="src/runtime/ciukidos_kernel_services.inc"
MONOLITH_SRC="src/boot/floppy_stage1.asm"
BUILD_SCRIPT="scripts/build_full.sh"
LOADER_BIN="build/full/obj/full_stage1.bin"
LOADER_LST="build/full/obj/full_stage1.lst"
KERNEL_BIN="build/full/obj/ciukidos.sys"
KERNEL_LST="build/full/obj/ciukidos.lst"

for required in \
  "$LOADER_SRC" "$KERNEL_SRC" "$SERVICES_SRC" "$MONOLITH_SRC" \
  "$BUILD_SCRIPT" "$LOADER_BIN" "$LOADER_LST" "$KERNEL_BIN" "$KERNEL_LST"; do
  [[ -s "$required" ]] || fail "missing or empty required file: $required"
done

rg -q '^STAGE1_SRC="src/boot/full_stage1_loader\.asm"$' "$BUILD_SCRIPT" \
  || fail "full build is not bound to the loader-only Stage1 source"
rg -q '^RUNTIME_SRC="src/runtime/ciukidos\.asm"$' "$BUILD_SCRIPT" \
  || fail "full build is not bound to the canonical CIUKIDOS kernel source"
rg -q '^%define CIUKIDOS_KERNEL_BUILD 1$' "$KERNEL_SRC" \
  || fail "canonical CIUKIDOS source does not enable the kernel build"

[[ ! -e src/runtime/ciukidos_kernel.asm ]] \
  || fail "obsolete alternate CIUKIDOS kernel source still exists"
[[ ! -e src/runtime/runtime.asm ]] \
  || fail "obsolete Stage1 runtime frontend still exists"

loader_size="$(stat -c%s "$LOADER_BIN")"
kernel_size="$(stat -c%s "$KERNEL_BIN")"
read -r expected_abi expected_service_count kernel_max runtime_seg \
  < <(python3 scripts/ciukidos_image.py --build-parameters)
(( loader_size > 0 && loader_size <= 0x1000 )) \
  || fail "loader size $loader_size is outside 1..4096 bytes"
(( kernel_size >= 124 && kernel_size <= kernel_max )) \
  || fail "kernel size $kernel_size exceeds the shared ABI extent"
python3 scripts/ciukidos_image.py --kernel "$KERNEL_BIN" --json \
  > build/full/obj/kernel-layout.json

if rg -n '^[[:space:]]*int[[:space:]]+(20h|21h|0x20|0x21)([[:space:]]|$)' "$LOADER_SRC" >/dev/null; then
  fail "loader-only Stage1 contains an INT 20h/21h instruction"
fi
for forbidden_symbol in \
  int20_handler int21_handler int21_exec int21_alloc int21_open int21_read int21_write; do
  if rg -q "^[[:space:]]*${forbidden_symbol}:" "$LOADER_SRC"; then
    fail "loader-only Stage1 defines normal DOS symbol $forbidden_symbol"
  fi
  if rg -q "[[:space:]]${forbidden_symbol}:" "$LOADER_LST"; then
    fail "loader binary listing contains normal DOS symbol $forbidden_symbol"
  fi
done

for required_symbol in \
  int20_handler int21_handler int21_exec int21_alloc int21_open int21_read int21_write ciukidos_image_end; do
  rg -q "[[:space:]]${required_symbol}:" "$KERNEL_LST" \
    || fail "CIUKIDOS kernel listing is missing $required_symbol"
done

if rg -n '^[[:space:]]*(jmp|call)[[:space:]]+far[[:space:]]+\[cs:(runtime_old_int21|old_int21)' \
  "$MONOLITH_SRC" "$SERVICES_SRC" >/dev/null; then
  fail "CIUKIDOS contains a far chain to the previous INT 21h owner"
fi

signature="$(read_hex "$KERNEL_BIN" 2 8)"
header_size="$(read_u16_le "$KERNEL_BIN" 10)"
abi_version="$(read_u16_le "$KERNEL_BIN" 12)"
service_count="$(read_u16_le "$KERNEL_BIN" 14)"
descriptor_size="$(read_u16_le "$KERNEL_BIN" 16)"
capabilities="$(read_u16_le "$KERNEL_BIN" 18)"
declared_size="$(read_u16_le "$KERNEL_BIN" 20)"
table_offset="$(read_u16_le "$KERNEL_BIN" 22)"
load_segment="$(read_u16_le "$KERNEL_BIN" 24)"

[[ "$signature" == "4349554b49444f53" ]] || fail "bad CIUKIDOS image signature: $signature"
(( header_size == 26 )) || fail "header size is $header_size, expected 26"
(( abi_version == 2 )) || fail "ABI version is $abi_version, expected 2"
(( service_count == expected_service_count )) || fail "service count disagrees with the shared ABI"
(( descriptor_size == 8 )) || fail "descriptor size is $descriptor_size, expected 8"
(( capabilities == 0x003f )) || fail "capabilities are $capabilities, expected 0x003f"
(( declared_size == kernel_size )) \
  || fail "header image size $declared_size does not match artifact size $kernel_size"
(( table_offset == 29 )) || fail "service table offset is $table_offset, expected 29"
(( load_segment == 0x0900 )) || fail "load segment is $load_segment, expected 0x0900"

table_magic="$(read_hex "$KERNEL_BIN" "$table_offset" 4)"
table_abi="$(read_u16_le "$KERNEL_BIN" $((table_offset + 4)))"
table_count="$(read_u16_le "$KERNEL_BIN" $((table_offset + 6)))"
table_descriptor_size="$(read_u16_le "$KERNEL_BIN" $((table_offset + 8)))"
[[ "$table_magic" == "52545356" ]] || fail "bad RTSV table magic: $table_magic"
(( table_abi == 2 )) || fail "RTSV ABI is $table_abi, expected 2"
(( table_count == service_count )) || fail "RTSV service count does not match image header"
(( table_descriptor_size == descriptor_size )) \
  || fail "RTSV descriptor size does not match image header"

descriptor_base=$((table_offset + 10))
descriptor_min=$((descriptor_base + service_count * descriptor_size))
for ((index = 0; index < service_count; index++)); do
  descriptor_offset=$((descriptor_base + index * descriptor_size))
  service_id="$(read_u16_le "$KERNEL_BIN" "$descriptor_offset")"
  service_version="$(read_u16_le "$KERNEL_BIN" $((descriptor_offset + 2)))"
  handler_offset="$(read_u16_le "$KERNEL_BIN" $((descriptor_offset + 4)))"
  reserved="$(read_u16_le "$KERNEL_BIN" $((descriptor_offset + 6)))"
  (( service_id == index + 1 )) \
    || fail "descriptor $index has service id $service_id, expected $((index + 1))"
  (( service_version == 1 )) \
    || fail "service $service_id has version $service_version, expected 1"
  (( handler_offset >= descriptor_min && handler_offset < kernel_size )) \
    || fail "service $service_id handler $handler_offset is outside the bounded image"
  (( reserved == 0 )) || fail "service $service_id reserved word is non-zero"
done

echo "[phase5-ownership] loader-only Stage1: PASS ($loader_size bytes, no DOS owner)"
echo "[phase5-ownership] CIUKIDOS ABI2: PASS ($kernel_size bytes, $service_count services, capabilities=0x003f)"
echo "[phase5-ownership] INT21 chain isolation: PASS (no legacy far-chain target)"
echo "[phase5-ownership] PASS"
