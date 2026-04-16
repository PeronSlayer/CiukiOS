#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
OUT_FILE="${1:-$PROJECT_DIR/third_party/freedos/runtime-manifest.csv}"

if [[ ! -d "$RUNTIME_DIR" ]]; then
    echo "Error: runtime directory not found: $RUNTIME_DIR" >&2
    exit 1
fi

tmp_file="$(mktemp)"
trap 'rm -f "$tmp_file"' EXIT

echo "path,size_bytes,sha256" > "$tmp_file"

while IFS= read -r -d '' file_path; do
    rel_path="${file_path#"$RUNTIME_DIR/"}"
    size_bytes="$(wc -c < "$file_path" | tr -d '[:space:]')"
    sha="$(sha256sum "$file_path" | awk '{print $1}')"
    echo "$rel_path,$size_bytes,$sha" >> "$tmp_file"
done < <(find "$RUNTIME_DIR" -type f ! -name '.gitkeep' -print0 | sort -z)

mv "$tmp_file" "$OUT_FILE"
trap - EXIT

echo "[DONE] runtime manifest generated: $OUT_FILE"
