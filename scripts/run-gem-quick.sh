#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
# Full rebuild via run_ciukios.sh (skip the QEMU run so we do it here).
export CIUKIOS_QEMU_SKIP_RUN=1
./run_ciukios.sh >/tmp/ciukios-build.log 2>&1 || { echo "BUILD FAIL"; tail -20 /tmp/ciukios-build.log; exit 1; }
printf 'gem\r\n' > /tmp/ciukios-autoexec.bat
mcopy -o -i build/ciukios.img /tmp/ciukios-autoexec.bat ::AUTOEXEC.BAT
rm -f build/debugcon.log build/serial-gem.log build/qemu.log
timeout 18 qemu-system-x86_64 \
  -machine q35 -m 512M -device virtio-vga \
  -debugcon file:build/debugcon.log -global isa-debugcon.iobase=0xe9 \
  -serial file:build/serial-gem.log \
  -d int,cpu_reset,guest_errors -D build/qemu.log \
  -no-reboot -no-shutdown -display none -monitor none \
  -boot order=c \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.4m.fd \
  -drive format=raw,file=build/ciukios.img
rc=$?
echo "QEMU exit=$rc"
echo "--- debugcon (hex tail) ---"
tail -c 400 build/debugcon.log | xxd | tail -20
echo "--- debugcon (ascii tail) ---"
tr -c '[:print:]\n' '.' < build/debugcon.log | tail -c 400
echo
echo "--- serial tail (last 40 lines) ---"
tail -40 build/serial-gem.log
echo "--- grep markers ---"
grep -nE "enter legacy_v86 loop|ms-trace|<end> reason|dispatch int=" build/serial-gem.log || true
