#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREECOM_DIR="$PROJECT_DIR/third_party/freedos/sources/freecom"
RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
MANIFEST="$PROJECT_DIR/third_party/freedos/manifest.csv"
LICENSES_DIR="$PROJECT_DIR/docs/legal/freedos-licenses"

FREECOM_ZIP_URL="${FREECOM_ZIP_URL:-https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/freecom.zip}"
FALLBACK_DOWNLOAD=1
DO_SYNC=1
LANGUAGE="english"

usage() {
    cat <<USAGE
Usage: $0 [--no-sync] [--no-fallback] [--language <lng>]

Options:
  --no-sync       Do not sync the FreeCOM source mirror before build
  --no-fallback   Fail if source build fails (disable package fallback)
  --language      FreeCOM build language (default: english)
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
        --no-sync)
            DO_SYNC=0
            shift
            ;;
        --no-fallback)
            FALLBACK_DOWNLOAD=0
            shift
            ;;
        --language)
            LANGUAGE="${2:-}"
            if [[ -z "$LANGUAGE" ]]; then
                echo "Error: --language requires a value" >&2
                exit 1
            fi
            shift 2
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

require_cmd git
require_cmd awk
require_cmd sha256sum
require_cmd make
require_cmd ia16-elf-gcc
require_cmd nasm

mkdir -p "$RUNTIME_DIR"
mkdir -p "$LICENSES_DIR"
mkdir -p "$PROJECT_DIR/build/logs"

if [[ "$DO_SYNC" == "1" ]]; then
    "$PROJECT_DIR/scripts/sync_freecom_repo.sh"
fi

if [[ ! -d "$FREECOM_DIR/.git" ]]; then
    echo "Error: FreeCOM source mirror not found: $FREECOM_DIR" >&2
    echo "Run: $PROJECT_DIR/scripts/sync_freecom_repo.sh" >&2
    exit 1
fi

gcc_include_dir="$(ia16-elf-gcc -print-file-name=include)"
if [[ ! -f "$gcc_include_dir/stdlib.h" ]]; then
    echo "[WARN] ia16-elf-gcc runtime headers not found (missing stdlib.h in $gcc_include_dir)"
    echo "[WARN] Source build may fail unless libi86/newlib headers are installed"
fi

build_log="$PROJECT_DIR/build/logs/freecom-build.log"
source_commit="$(git -C "$FREECOM_DIR" rev-parse HEAD)"

build_from_source() {
    echo "[INFO] Building FreeCOM from source (language=$LANGUAGE)"
    if ! (
        cd "$FREECOM_DIR"
        LNG="$LANGUAGE" ./build.sh gcc
    ) >"$build_log" 2>&1; then
        return 1
    fi

    local built="$FREECOM_DIR/command.com"
    if [[ ! -f "$built" ]]; then
        echo "Error: source build completed but command.com not found" >&2
        return 1
    fi

    cp "$built" "$RUNTIME_DIR/COMMAND.COM"

    if [[ -f "$FREECOM_DIR/license" ]]; then
        cp "$FREECOM_DIR/license" "$LICENSES_DIR/freecom-license.txt"
    fi

    echo "[DONE] FreeCOM source build succeeded"
    echo "- command: $RUNTIME_DIR/COMMAND.COM"
    echo "- log:     $build_log"
    return 0
}

fallback_download_command_com() {
    require_cmd unzip

    (
        set -euo pipefail
        local tmp_dir
        local zip_path
        local entry
        local extracted

        tmp_dir="$(mktemp -d)"
        trap 'rm -rf "$tmp_dir"' EXIT
        zip_path="$tmp_dir/freecom.zip"

        echo "[INFO] Downloading fallback package: $FREECOM_ZIP_URL"
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL "$FREECOM_ZIP_URL" -o "$zip_path"
        elif command -v wget >/dev/null 2>&1; then
            wget -q "$FREECOM_ZIP_URL" -O "$zip_path"
        else
            echo "Error: missing curl/wget for fallback download" >&2
            exit 1
        fi

        entry="$(unzip -Z1 "$zip_path" | awk '{ low = tolower($0); if (low ~ /command[.]com$/) { print; exit } }')"
        if [[ -z "$entry" ]]; then
            echo "Error: command.com not found inside fallback package" >&2
            exit 1
        fi

        unzip -j -o "$zip_path" "$entry" -d "$tmp_dir" >/dev/null

        extracted="$(find "$tmp_dir" -maxdepth 1 -type f -iname 'command.com' | head -n 1 || true)"
        if [[ -z "$extracted" || ! -f "$extracted" ]]; then
            echo "Error: fallback extraction did not produce command.com" >&2
            exit 1
        fi

        cp "$extracted" "$RUNTIME_DIR/COMMAND.COM"

        if [[ -f "$FREECOM_DIR/license" ]]; then
            cp "$FREECOM_DIR/license" "$LICENSES_DIR/freecom-license.txt"
        fi
    )

    echo "[DONE] Fallback package import succeeded"
    echo "- command: $RUNTIME_DIR/COMMAND.COM"
    echo "- source:  $FREECOM_ZIP_URL"
    return 0
}

notes=""
source_path=""

if build_from_source; then
    notes="built-from-freecom-source:$source_commit"
    source_path="$FREECOM_DIR/command.com"
else
    echo "[WARN] FreeCOM source build failed"
    echo "[WARN] Build log: $build_log"

    if [[ "$FALLBACK_DOWNLOAD" != "1" ]]; then
        echo "Error: fallback is disabled and source build failed" >&2
        exit 1
    fi

    fallback_download_command_com
    notes="fallback-from-freecom-zip:$source_commit"
    source_path="$FREECOM_ZIP_URL"
fi

sha="$(sha256sum "$RUNTIME_DIR/COMMAND.COM" | awk '{print $1}')"

if [[ -f "$MANIFEST" ]]; then
    tmp_manifest="$(mktemp)"
    trap 'rm -f "$tmp_manifest"' EXIT

    awk -F',' -v OFS=',' \
        -v sha="$sha" \
        -v src="$source_path" \
        -v note="$notes" '
        NR==1 {
            print $0
            next
        }
        {
            if ($1 == "core" && $2 == "COMMAND.COM") {
                print "core","COMMAND.COM","yes","yes",sha,src,"GPL-2.0-or-later",note
                found = 1
            } else {
                print $0
            }
        }
        END {
            if (!found) {
                print "core","COMMAND.COM","yes","yes",sha,src,"GPL-2.0-or-later",note
            }
        }
    ' "$MANIFEST" > "$tmp_manifest"

    mv "$tmp_manifest" "$MANIFEST"
    trap - EXIT
fi

echo "[DONE] FreeCOM integration complete"
echo "- command:  $RUNTIME_DIR/COMMAND.COM"
echo "- sha256:   $sha"
echo "- manifest: $MANIFEST"
