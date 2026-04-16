#!/usr/bin/env bash
set -euo pipefail

# import_ozonegui.sh — Import oZone GUI runtime files into CiukiOS FreeDOS bundle
# Usage: ./scripts/import_ozonegui.sh --source /path/to/extracted/ozone
#
# Inputs:  Directory containing extracted oZone files (OZONE.EXE, etc.)
# Outputs: Copies to third_party/freedos/runtime/OZONE/ and updates manifest.csv

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFEST="$PROJECT_DIR/third_party/freedos/manifest.csv"
DEST_DIR="$PROJECT_DIR/third_party/freedos/runtime/OZONE"

SOURCE_DIR=""

usage() {
    echo "Usage: $0 --source <dir>"
    echo ""
    echo "  --source <dir>   Directory containing extracted oZone files"
    echo ""
    echo "The script copies oZone runtime files into:"
    echo "  $DEST_DIR"
    echo "and updates manifest.csv with checksums."
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            SOURCE_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "[ERROR] Unknown argument: $1"
            usage
            ;;
    esac
done

if [[ -z "$SOURCE_DIR" ]]; then
    echo "[ERROR] --source is required"
    usage
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "[ERROR] Source directory does not exist: $SOURCE_DIR"
    exit 1
fi

# Required tools
for tool in find cp sha256sum; do
    if ! command -v "$tool" &>/dev/null; then
        echo "[ERROR] Required tool not found: $tool"
        exit 1
    fi
done

# Create destination
mkdir -p "$DEST_DIR"

# Known oZone files to import (case-insensitive search)
# Format: filename required(yes/no)
OZONE_FILES=(
    "OZONE.EXE:yes"
    "OZONE.INI:no"
    "OZONE.ICO:no"
    "OZONE.HLP:no"
    "README.TXT:no"
    "COPYING:no"
    "LICENSE:no"
)

IMPORT_COUNT=0
MISSING_REQUIRED=0

# Helper: find file case-insensitively
find_file_ci() {
    local dir="$1"
    local name="$2"
    find "$dir" -maxdepth 3 -iname "$name" -type f 2>/dev/null | head -n1
}

# Helper: update or append manifest row for ozonegui component
update_manifest_row() {
    local fname="$1"
    local imported="$2"
    local sha="$3"
    local notes="$4"
    local source_url="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/gui/ozonegui.zip"
    local license="GPL-2.0-or-later?"

    # Remove existing row for this file if present
    if grep -q "^ozonegui,$fname," "$MANIFEST" 2>/dev/null; then
        local tmp
        tmp=$(mktemp)
        grep -v "^ozonegui,$fname," "$MANIFEST" > "$tmp"
        mv "$tmp" "$MANIFEST"
    fi

    # Append new row
    echo "ozonegui,$fname,no,$imported,$sha,$source_url,$license,$notes" >> "$MANIFEST"
}

echo "[import-ozone] source: $SOURCE_DIR"
echo "[import-ozone] destination: $DEST_DIR"
echo ""

for entry in "${OZONE_FILES[@]}"; do
    IFS=':' read -r fname required <<< "$entry"
    found_path=$(find_file_ci "$SOURCE_DIR" "$fname")

    if [[ -n "$found_path" ]]; then
        cp "$found_path" "$DEST_DIR/$fname"
        sha=$(sha256sum "$DEST_DIR/$fname" | awk '{print $1}')
        echo "[OK] imported: $fname (sha256=$sha)"
        update_manifest_row "$fname" "yes" "$sha" "imported"
        ((IMPORT_COUNT++)) || true
    else
        if [[ "$required" == "yes" ]]; then
            echo "[WARN] REQUIRED file not found: $fname"
            ((MISSING_REQUIRED++)) || true
            update_manifest_row "$fname" "no" "" "missing-required"
        else
            echo "[INFO] optional file not found: $fname (skipped)"
        fi
    fi
done

# Also copy any additional .EXE, .COM, .SYS files found in source
echo ""
echo "[import-ozone] scanning for additional runtime files..."
while IFS= read -r -d '' extra_file; do
    base=$(basename "$extra_file")
    base_upper=$(echo "$base" | tr '[:lower:]' '[:upper:]')

    # Skip files we already handled
    skip=0
    for entry in "${OZONE_FILES[@]}"; do
        IFS=':' read -r known_name _ <<< "$entry"
        if [[ "$base_upper" == "$known_name" ]]; then
            skip=1
            break
        fi
    done

    if [[ "$skip" -eq 0 ]]; then
        cp "$extra_file" "$DEST_DIR/$base_upper"
        sha=$(sha256sum "$DEST_DIR/$base_upper" | awk '{print $1}')
        echo "[OK] extra: $base_upper (sha256=$sha)"
        update_manifest_row "$base_upper" "yes" "$sha" "imported-extra"
        ((IMPORT_COUNT++)) || true
    fi
done < <(find "$SOURCE_DIR" -maxdepth 3 \( -iname '*.exe' -o -iname '*.com' -o -iname '*.sys' -o -iname '*.ovl' -o -iname '*.dat' -o -iname '*.cfg' \) -type f -print0 2>/dev/null)

echo ""
echo "[import-ozone] import complete: $IMPORT_COUNT files imported"

if [[ "$MISSING_REQUIRED" -gt 0 ]]; then
    echo "[WARN] $MISSING_REQUIRED required file(s) missing"
    exit 1
fi

echo "[PASS] oZone import successful"
