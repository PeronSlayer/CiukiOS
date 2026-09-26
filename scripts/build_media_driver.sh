#!/usr/bin/env bash
set -euo pipefail
out="${1:-build/full/obj}"
for tool in ia16-elf-gcc ia16-elf-ld ia16-elf-objcopy nasm; do
    command -v "$tool" >/dev/null || { echo "[build-media-driver] ERROR: missing $tool" >&2; exit 1; }
done
mkdir -p "$out"
ia16-elf-gcc -DMEDIA_MODULE=1 -mcmodel=tiny -march=i286 -Os -ffreestanding -fno-builtin -fno-common -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -c src/com/media.c -o "$out/media_driver.o"
nasm -DMEDIA_DRIVER=1 -f elf32 src/com/media_bios.asm -o "$out/media_driver_bios.o"
nasm -f elf32 src/com/media_driver_entry.asm -o "$out/media_driver_entry.o"
ia16-elf-ld --gc-sections -T src/com/media_driver.ld -Map "$out/media_driver.map" -o "$out/media_driver.elf" "$out/media_driver_entry.o" "$out/media_driver_bios.o" "$out/media_driver.o" "$(ia16-elf-gcc -print-libgcc-file-name)"
ia16-elf-objcopy -O binary "$out/media_driver.elf" "$out/media.drv"
echo "[build-media-driver] MEDIA.DRV: $(wc -c < "$out/media.drv") bytes"
