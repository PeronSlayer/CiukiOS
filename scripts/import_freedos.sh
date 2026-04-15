#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="$PROJECT_DIR/third_party/freedos/runtime"
MANIFEST="$PROJECT_DIR/third_party/freedos/manifest.csv"

SOURCE_DIR=""
CLEAN_FIRST=0

usage() {
    cat <<USAGE
Usage: $0 --source <freedos-files-dir> [--clean]

Options:
  --source <dir>   Source directory containing extracted FreeDOS files
  --clean          Remove existing runtime files before import
USAGE
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: missing command: $1" >&2
        exit 1
    }
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            SOURCE_DIR="${2:-}"
            shift 2
            ;;
        --clean)
            CLEAN_FIRST=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "$SOURCE_DIR" ]]; then
    echo "Error: --source is required" >&2
    usage
    exit 1
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "Error: source directory does not exist: $SOURCE_DIR" >&2
    exit 1
fi

require_cmd find
require_cmd cp
require_cmd sha256sum

mkdir -p "$DEST_DIR"

if [[ "$CLEAN_FIRST" == "1" ]]; then
    find "$DEST_DIR" -maxdepth 1 -type f ! -name ".gitkeep" -delete
fi

tmp_manifest="$(mktemp)"
trap 'rm -f "$tmp_manifest"' EXIT

echo "component,file_name,required,imported,sha256,source_path,license,notes" > "$tmp_manifest"

copy_component() {
    local component="$1"
    local name="$2"
    local required="$3"
    local license_hint="$4"
    local src
    local sha

    src="$(find "$SOURCE_DIR" -type f -iname "$name" | head -n 1 || true)"

    if [[ -n "$src" ]]; then
        cp "$src" "$DEST_DIR/$name"
        sha="$(sha256sum "$DEST_DIR/$name" | awk '{print $1}')"
        echo "$component,$name,$required,yes,$sha,$src,$license_hint,imported" >> "$tmp_manifest"
        echo "[OK] imported $name"
    else
        echo "$component,$name,$required,no,,,${license_hint},missing" >> "$tmp_manifest"
        if [[ "$required" == "yes" ]]; then
            echo "[WARN] missing required: $name"
        else
            echo "[INFO] missing optional: $name"
        fi
    fi
}

copy_component "core"   "COMMAND.COM"  "yes" "GPL-2.0-or-later?"
copy_component "core"   "KERNEL.SYS"   "yes" "GPL-2.0-or-later?"
copy_component "core"   "FDCONFIG.SYS" "yes" "GPL-2.0-or-later?"
copy_component "core"   "FDAUTO.BAT"   "yes" "GPL-2.0-or-later?"
copy_component "memory" "HIMEMX.EXE"   "no"  "GPL-2.0-or-later?"
copy_component "memory" "JEMM386.EXE"  "no"  "GPL-2.0-or-later?"
copy_component "utils"  "MEM.EXE"      "no"  "GPL-2.0-or-later?"
copy_component "utils"  "MODE.COM"     "no"  "GPL-2.0-or-later?"
copy_component "utils"  "XCOPY.EXE"    "no"  "GPL-2.0-or-later?"

mv "$tmp_manifest" "$MANIFEST"
trap - EXIT
touch "$DEST_DIR/.gitkeep"

echo "[DONE] FreeDOS import complete"
echo "- runtime dir: $DEST_DIR"
echo "- manifest:    $MANIFEST"
