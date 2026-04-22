#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
printf 'gem vdi\r\n' > /tmp/ciukios-autoexec.bat
mcopy -o -i build/ciukios.img /tmp/ciukios-autoexec.bat ::AUTOEXEC.BAT
rm -f build/debugcon.log build/serial-gem.log build/qemu.log
timeout 18 qemu-system-x86_64 \
  -machine q35 -m 512M -device virtio-vga \
  -debugcon file:build/debugcon.log -global isa-debugcon.iobase=0xe9 \
  -serial file:build/serial-gem.log \
  -no-reboot -no-shutdown -display none -monitor none \
  -boot order=c \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.4m.fd \
  -drive format=raw,file=build/ciukios.img </dev/null >/dev/null 2>&1
echo "QEMU exit rc=$?"
