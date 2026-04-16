#!/usr/bin/env bash
set -euo pipefail

# import_opengem.sh — Import OpenGEM GUI runtime files into CiukiOS FreeDOS bundle
# Usage:
#   ./scripts/import_opengem.sh                            # uses default zip location
#   ./scripts/import_opengem.sh --zip /path/to/opengem.zip
#   ./scripts/import_opengem.sh --source /path/to/extracted/dir
#
# Inputs:  ZIP archive or extracted directory containing OpenGEM files
# Outputs: Copies to third_party/freedos/runtime/OPENGEM/ and updates manifest.csv

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFEST="$PROJECT_DIR/third_party/freedos/manifest.csv"
DEST_DIR="$PROJECT_DIR/third_party/freedos/runtime/OPENGEM"

DEFAULT_ZIP="$PROJECT_DIR/third_party/freedos/sources/opengem/opengem.zip"
ZIP_PATH=""
SOURCE_DIR=""

usage() {
    echo "Usage: $0 [--zip <path>] [--source <dir>]"
    echo ""
    echo "  --zip <path>     Path to opengem.zip (default: $DEFAULT_ZIP)"
    echo "  --source <dir>   Directory containing extracted OpenGEM files"
    echo ""
    echo "If neither --zip nor --source is given, the default zip path is used."
    echo ""
    echo "The script copies OpenGEM runtime files into:"
    echo "  $DEST_DIR"
    echo "and updates manifest.csv with checksums."
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --zip)
            ZIP_PATH="$2"
            shift 2
            ;;
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

# If neither specified, use default zip
if [[ -z "$ZIP_PATH" ]] && [[ -z "$SOURCE_DIR" ]]; then
    ZIP_PATH="$DEFAULT_ZIP"
fi

# Required tools
for tool in find cp sha256sum; do
    if ! command -v "$tool" &>/dev/null; then
        echo "[ERROR] Required tool not found: $tool"
        exit 1
    fi
done

# If using zip, extract to temp dir
TEMP_DIR=""
if [[ -n "$ZIP_PATH" ]]; then
    if ! command -v unzip &>/dev/null; then
        echo "[ERROR] Required tool not found: unzip"
        exit 1
    fi
    if [[ ! -f "$ZIP_PATH" ]]; then
        echo "[ERROR] ZIP file not found: $ZIP_PATH"
        echo ""
        echo "To obtain OpenGEM:"
        echo "  Download opengem.zip from the FreeDOS 1.3 repository"
        echo "  Place it at: $DEFAULT_ZIP"
        exit 1
    fi
    TEMP_DIR="$(mktemp -d)"
    trap 'rm -rf "$TEMP_DIR"' EXIT
    echo "[import-opengem] extracting: $ZIP_PATH"
    unzip -q -o "$ZIP_PATH" -d "$TEMP_DIR"
    SOURCE_DIR="$TEMP_DIR"
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "[ERROR] Source directory does not exist: $SOURCE_DIR"
    exit 1
fi

# Locate the OpenGEM root inside the extracted tree
# Expected structure: GUI/OPENGEM/ or just OPENGEM/ or flat
OPENGEM_ROOT=""
for candidate in \
    "$SOURCE_DIR/GUI/OPENGEM" \
    "$SOURCE_DIR/gui/opengem" \
    "$SOURCE_DIR/OPENGEM" \
    "$SOURCE_DIR/opengem" \
    "$SOURCE_DIR"; do
    if [[ -d "$candidate" ]]; then
        # Verify it looks like an OpenGEM tree (has GEMAPPS or a launch candidate)
        if [[ -d "$candidate/GEMAPPS" ]] || [[ -d "$candidate/gemapps" ]] || \
           find "$candidate" -maxdepth 1 -iname "GEM.BAT" -type f 2>/dev/null | grep -q .; then
            OPENGEM_ROOT="$candidate"
            break
        fi
    fi
done

if [[ -z "$OPENGEM_ROOT" ]]; then
    echo "[ERROR] Could not locate OpenGEM root in source tree."
    echo "Expected to find GUI/OPENGEM/ with GEMAPPS/ subdirectory."
    exit 1
fi

echo "[import-opengem] source root: $OPENGEM_ROOT"
echo "[import-opengem] destination: $DEST_DIR"
echo ""

# Create destination
mkdir -p "$DEST_DIR"

# Copy entire OpenGEM tree preserving structure
echo "[import-opengem] copying runtime tree..."
cp -a "$OPENGEM_ROOT"/. "$DEST_DIR"/

# Normalize: ensure key directories/files are uppercase at top level
# (deep tree stays as-is since DOS FAT is case-insensitive)
for f in "$DEST_DIR"/*; do
    base="$(basename "$f")"
    upper="$(echo "$base" | tr '[:lower:]' '[:upper:]')"
    if [[ "$base" != "$upper" ]] && [[ ! -e "$DEST_DIR/$upper" ]]; then
        mv "$f" "$DEST_DIR/$upper"
    fi
done

IMPORT_COUNT=0

# Count imported files
while IFS= read -r -d '' _file; do
    ((IMPORT_COUNT++)) || true
done < <(find "$DEST_DIR" -type f -print0 2>/dev/null)

echo "[import-opengem] $IMPORT_COUNT files in runtime tree"
echo ""

# Detect candidate launch entry in priority order
LAUNCH_CANDIDATES=(
    "GEM.BAT"
    "GEM.EXE"
    "DESKTOP.APP"
    "OPENGEM.BAT"
    "OPENGEM.EXE"
)

FOUND_ENTRY=""
FOUND_ENTRY_REL=""
for cand in "${LAUNCH_CANDIDATES[@]}"; do
    hit=$(find "$DEST_DIR" -maxdepth 3 -iname "$cand" -type f 2>/dev/null | head -n1)
    if [[ -n "$hit" ]]; then
        FOUND_ENTRY="$hit"
        FOUND_ENTRY_REL="${hit#"$DEST_DIR/"}"
        echo "[OK] launch entry found: $FOUND_ENTRY_REL (candidate: $cand)"
        break
    fi
done

if [[ -z "$FOUND_ENTRY" ]]; then
    echo "[FAIL] No runnable launch entry found in OpenGEM tree!"
    echo "Searched for: ${LAUNCH_CANDIDATES[*]}"
    echo ""
    echo "Please verify the archive contents and try again."
    rm -rf "$DEST_DIR"
    exit 1
fi

# Compute sha256 for key files
echo ""
echo "[import-opengem] computing checksums for manifest..."

# Helper: update or append manifest row for opengem component
update_manifest_row() {
    local fname="$1"
    local imported="$2"
    local sha="$3"
    local notes="$4"
    local source_url="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/gui/opengem.zip"
    local license="GPL-2.0"

    # Remove existing row for this file if present
    if grep -q "^opengem,$fname," "$MANIFEST" 2>/dev/null; then
        local tmp
        tmp=$(mktemp)
        grep -v "^opengem,$fname," "$MANIFEST" > "$tmp"
        mv "$tmp" "$MANIFEST"
    fi

    # Append new row
    echo "opengem,$fname,no,$imported,$sha,$source_url,$license,$notes" >> "$MANIFEST"
}

# Record launch entry
sha_entry=$(sha256sum "$FOUND_ENTRY" | awk '{print $1}')
update_manifest_row "$FOUND_ENTRY_REL" "yes" "$sha_entry" "launch-entry"
echo "[OK] manifest: $FOUND_ENTRY_REL (launch-entry)"

# Record key executables
KEY_EXES=()
while IFS= read -r -d '' exe_file; do
    rel="${exe_file#"$DEST_DIR/"}"
    # Skip if it's the launch entry we already recorded
    if [[ "$rel" == "$FOUND_ENTRY_REL" ]]; then
        continue
    fi
    KEY_EXES+=("$exe_file")
done < <(find "$DEST_DIR" -maxdepth 4 \( -iname '*.exe' -o -iname '*.app' -o -iname '*.com' -o -iname '*.bat' \) -type f -print0 2>/dev/null | sort -z)

for exe_file in "${KEY_EXES[@]}"; do
    rel="${exe_file#"$DEST_DIR/"}"
    sha=$(sha256sum "$exe_file" | awk '{print $1}')
    update_manifest_row "$rel" "yes" "$sha" "imported"
    echo "[OK] manifest: $rel"
done

echo ""
echo "========================================"
echo "[PASS] OpenGEM import successful"
echo "  Files imported: $IMPORT_COUNT"
echo "  Launch entry:   $FOUND_ENTRY_REL"
echo "  Destination:    $DEST_DIR"
echo "========================================"
echo ""
echo "Next commands:"
echo "  # Build and run with OpenGEM:"
echo "  CIUKIOS_INCLUDE_OPENGEM=1 ./run_ciukios.sh"
echo ""
echo "  # Validate pipeline:"
echo "  make test-freedos-pipeline"
echo ""
echo "  # At the CiukiOS shell prompt:"
echo "  C:\\> opengem"
