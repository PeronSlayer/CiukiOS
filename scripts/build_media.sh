#!/usr/bin/env bash
set -euo pipefail
out="${1:-build/full/obj}"
for tool in ia16-elf-gcc ia16-elf-ld ia16-elf-objcopy nasm; do
    command -v "$tool" >/dev/null || { echo "[build-media] ERROR: required tool is missing: $tool" >&2; exit 1; }
done
mkdir -p "$out"
ia16-elf-gcc -mcmodel=tiny -march=i286 -Os -ffreestanding -fno-builtin -fno-common -Wall -Wextra -Werror -c src/com/media.c -o "$out/media.o"
nasm -f elf32 src/com/media_bios.asm -o "$out/media_bios.o"
ia16-elf-ld -T src/com/media.ld -Map "$out/media.map" -o "$out/media.elf" "$out/media_bios.o" "$out/media.o" "$(ia16-elf-gcc -print-libgcc-file-name)"
ia16-elf-objcopy -O binary "$out/media.elf" "$out/media.com"
media_bytes="$(wc -c < "$out/media.com")"
if (( media_bytes < 256 || media_bytes >= 0xee00 )); then
    echo "[build-media] ERROR: invalid COM size: $media_bytes" >&2
    exit 1
fi
echo "[build-media] MEDIA.COM: $media_bytes bytes"
