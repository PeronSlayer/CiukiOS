#!/usr/bin/env bash
set -euo pipefail

# Default CIUKIOS_ROOT to the repository root (parent of scripts/) when not set externally.
: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

: "${IMG:=$CIUKIOS_ROOT/build/full/ciukios-full.img}"
: "${DRIVERS_SRC_DIR:=$CIUKIOS_ROOT/third_party/drivers}"
: "${DRIVERS_IMAGE_DIR:=::SYSTEM/DRIVERS}"

if [[ "$IMG" != /* ]]; then
	IMG="$CIUKIOS_ROOT/$IMG"
fi

if [[ "$DRIVERS_SRC_DIR" != /* ]]; then
	DRIVERS_SRC_DIR="$CIUKIOS_ROOT/$DRIVERS_SRC_DIR"
fi

fail_verify() {
	echo "[verify-full-drivers] FAIL $1" >&2
	exit 1
}

payload_manifest_from_dir() {
	local payload_dir="$1"
	local manifest_file="$2"
	local payload_file
	: > "$manifest_file"

	while IFS= read -r -d '' payload_file; do
		local file_size
		local file_hash
		file_size="$(stat -c%s "$payload_file")"
		file_hash="$(sha256sum "$payload_file" | awk '{print $1}')"
		printf '%s %s\n' "$file_hash" "$file_size" >> "$manifest_file"
	done < <(find "$payload_dir" -type f -print0)

	sort -o "$manifest_file" "$manifest_file"
}

payload_stats_from_manifest() {
	local manifest_file="$1"
	awk '
		{ files += 1; bytes += $2 }
		END { printf "%d %d\n", files, bytes }
	' "$manifest_file"
}

if [[ ! -f "$IMG" ]]; then
	fail_verify "image not found: $IMG"
fi

if [[ ! -d "$DRIVERS_SRC_DIR" ]]; then
	fail_verify "source directory not found: $DRIVERS_SRC_DIR"
fi

if ! command -v mdir >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
	fail_verify "mtools (mdir/mcopy) missing"
fi

if ! command -v sha256sum >/dev/null 2>&1; then
	fail_verify "sha256sum missing"
fi

drivers_image_dir="${DRIVERS_IMAGE_DIR%/}"
if [[ -z "$drivers_image_dir" ]]; then
	fail_verify "invalid image payload directory: $DRIVERS_IMAGE_DIR"
fi

if ! mdir -i "$IMG" "$drivers_image_dir" >/dev/null 2>&1; then
	fail_verify "directory missing in image: $drivers_image_dir"
fi

tmp_dir="$(mktemp -d)"
cleanup() {
	rm -rf "$tmp_dir"
}
trap cleanup EXIT

if ! mcopy -s -n -i "$IMG" "$drivers_image_dir" "$tmp_dir/" >/dev/null 2>&1; then
	fail_verify "unable to copy image payload from: $drivers_image_dir"
fi

drivers_leaf="${drivers_image_dir##*/}"
extracted_dir="$tmp_dir/$drivers_leaf"
if [[ ! -d "$extracted_dir" ]]; then
	fail_verify "copied payload directory missing: $extracted_dir"
fi

src_manifest="$tmp_dir/src_manifest.txt"
img_manifest="$tmp_dir/img_manifest.txt"
payload_manifest_from_dir "$DRIVERS_SRC_DIR" "$src_manifest"
payload_manifest_from_dir "$extracted_dir" "$img_manifest"

read -r src_files src_bytes < <(payload_stats_from_manifest "$src_manifest")
read -r img_files img_bytes < <(payload_stats_from_manifest "$img_manifest")

if ! cmp -s "$src_manifest" "$img_manifest"; then
	missing_manifest="$tmp_dir/missing_or_changed.txt"
	extra_manifest="$tmp_dir/extra_or_changed.txt"
	comm -23 "$src_manifest" "$img_manifest" > "$missing_manifest" || true
	comm -13 "$src_manifest" "$img_manifest" > "$extra_manifest" || true
	missing_count="$(wc -l < "$missing_manifest")"
	extra_count="$(wc -l < "$extra_manifest")"
	echo "[verify-full-drivers] FAIL content-mismatch src_files=$src_files image_files=$img_files src_bytes=$src_bytes image_bytes=$img_bytes missing_or_changed=$missing_count extra_or_changed=$extra_count dir=$drivers_image_dir" >&2
	exit 1
fi

echo "[verify-full-drivers] PASS files=$src_files bytes=$src_bytes hashes=match dir=$drivers_image_dir"