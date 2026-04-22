#!/usr/bin/env sh
set -eu

INPUT=""
OUTPUT=""
MAX_DIM="768"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --input)
            INPUT="$2"
            shift 2
            ;;
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        --max-dim)
            MAX_DIM="$2"
            shift 2
            ;;
        *)
            echo "usage: $0 --input <png> --output <c-file> [--max-dim <N>]" >&2
            exit 1
            ;;
    esac
done

if [ -z "$INPUT" ] || [ -z "$OUTPUT" ]; then
    echo "usage: $0 --input <png> --output <c-file> [--max-dim <N>]" >&2
    exit 1
fi

if [ ! -f "$INPUT" ]; then
    echo "error: input image not found: $INPUT" >&2
    exit 1
fi

if command -v magick >/dev/null 2>&1; then
    CONVERT_TOOL="magick"
    IDENTIFY_CMD="magick identify"
elif command -v convert >/dev/null 2>&1 && command -v identify >/dev/null 2>&1; then
    CONVERT_TOOL="convert"
    IDENTIFY_CMD="identify"
else
    echo "error: ImageMagick not found (need 'magick' or 'convert' + 'identify')" >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

TMP_PNG="$TMP_DIR/splash.png"
TMP_RAW="$TMP_DIR/splash.rgba"
TMP_ARRAY="$TMP_DIR/splash_array.c"
TMP_OUT="$TMP_DIR/splash_data.c"

if [ "$CONVERT_TOOL" = "magick" ]; then
    magick "$INPUT" -auto-orient -resize "${MAX_DIM}x${MAX_DIM}>" "$TMP_PNG"
else
    convert "$INPUT" -auto-orient -resize "${MAX_DIM}x${MAX_DIM}>" "$TMP_PNG"
fi

set -- $($IDENTIFY_CMD -format "%w %h" "$TMP_PNG")
WIDTH="$1"
HEIGHT="$2"

if [ "$CONVERT_TOOL" = "magick" ]; then
    magick "$TMP_PNG" -alpha on -depth 8 RGBA:"$TMP_RAW"
else
    convert "$TMP_PNG" -alpha on -depth 8 RGBA:"$TMP_RAW"
fi

xxd -i -n stage2_splash_image_rgba "$TMP_RAW" > "$TMP_ARRAY"

{
    echo "/* Auto-generated from: $INPUT */"
    echo "/* Resized to: ${WIDTH}x${HEIGHT}, format: RGBA8888 */"
    echo "unsigned int stage2_splash_image_width = ${WIDTH}U;"
    echo "unsigned int stage2_splash_image_height = ${HEIGHT}U;"
    cat "$TMP_ARRAY"
} > "$TMP_OUT"

mkdir -p "$(dirname "$OUTPUT")"
mv "$TMP_OUT" "$OUTPUT"

echo "[ok] generated splash C asset: $OUTPUT (${WIDTH}x${HEIGHT})"
