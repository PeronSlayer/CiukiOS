#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREECOM_DIR="$PROJECT_DIR/third_party/freedos/sources/freecom"
LOCK_FILE="$PROJECT_DIR/third_party/freedos/upstreams.lock"
OPENGEM_ZIP_DEFAULT="$PROJECT_DIR/third_party/freedos/sources/opengem/opengem.zip"
OPENGEM_ZIP_PATH="${OPENGEM_ZIP_PATH:-$OPENGEM_ZIP_DEFAULT}"

"$PROJECT_DIR/scripts/sync_freecom_repo.sh"

if [[ ! -d "$FREECOM_DIR/.git" ]]; then
    echo "Error: freecom repo not available after sync: $FREECOM_DIR" >&2
    exit 1
fi

freecom_repo="$(git -C "$FREECOM_DIR" config --get remote.origin.url)"
freecom_commit="$(git -C "$FREECOM_DIR" rev-parse HEAD)"

opengem_sha=""
if [[ -f "$OPENGEM_ZIP_PATH" ]]; then
    opengem_sha="$(sha256sum "$OPENGEM_ZIP_PATH" | awk '{print $1}')"
fi

tmp_file="$(mktemp)"
trap 'rm -f "$tmp_file"' EXIT

{
    echo "freecom.repo=$freecom_repo"
    echo "freecom.commit=$freecom_commit"
    echo "opengem.zip.path=${OPENGEM_ZIP_PATH#$PROJECT_DIR/}"
    echo "opengem.zip.sha256=$opengem_sha"
} > "$tmp_file"

mv "$tmp_file" "$LOCK_FILE"
trap - EXIT

echo "[DONE] FreeDOS upstream lock updated: $LOCK_FILE"
