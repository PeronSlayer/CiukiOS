#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREECOM_URL="${FREECOM_REPO_URL:-https://github.com/FDOS/freecom.git}"
FREECOM_DIR="$PROJECT_DIR/third_party/freedos/sources/freecom"
MANIFEST="$PROJECT_DIR/third_party/freedos/manifest.csv"

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Error: missing command: $1" >&2
        exit 1
    }
}

require_cmd git
require_cmd awk

if [[ ! -d "$FREECOM_DIR/.git" ]]; then
    mkdir -p "$(dirname "$FREECOM_DIR")"
    git clone --depth 1 "$FREECOM_URL" "$FREECOM_DIR"
else
    git -C "$FREECOM_DIR" fetch --depth 1 origin
    git -C "$FREECOM_DIR" reset --hard origin/HEAD
fi

commit="$(git -C "$FREECOM_DIR" rev-parse HEAD)"

if [[ -f "$MANIFEST" ]]; then
    tmp="$(mktemp)"
    trap 'rm -f "$tmp"' EXIT

    awk -F',' 'NR==1 || $1 != "source"' "$MANIFEST" > "$tmp"
    echo "source,freecom.git,no,yes,$commit,$FREECOM_URL,GPL-2.0-or-later?,repo-synced" >> "$tmp"
    mv "$tmp" "$MANIFEST"
    trap - EXIT
fi

echo "[DONE] freecom synced"
echo "- repo:   $FREECOM_URL"
echo "- path:   $FREECOM_DIR"
echo "- commit: $commit"
