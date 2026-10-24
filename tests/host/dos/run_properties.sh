#!/usr/bin/env bash
# G17 focused host-only production DOS VM fixtures, canonical host flags.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/g17-vm"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1
python3 "$root/tests/host/vm/prepare_lifecycle.py" "$root" "$out"
for unit in vm_source opl3 mixer; do
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -include "$root/tests/host/vm_audio_shim.h" -I "$root/src/kernel/include" \
        -c "$root/src/kernel/audio/$unit.c" -o "$out/dosvm_audio_$unit.o"
done
pool="$(python3 "$root/tests/host/vm/pool_build.py" "$root" "$out/vm")"
network_sources=("$root/src/kernel/net/bridge.c" "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/vm/dosvm_ne2000.c")
guest_cpu=()
if [[ -f /usr/include/x86emu.h ]]; then
    guest_cpu=(-DCIUKI_DOSVM_GUEST_CPU "$root/tests/host/dosvm_guest_cpu.c" -lx86emu)
fi
clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" -I "$out" "$root/tests/host/dosvm_test.c" \
    "$root/tests/host/dosvm_uaddr_model.c" "${network_sources[@]}" \
    "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" \
    "$out/dosvm_audio_vm_source.o" "$out/dosvm_audio_opl3.o" "$out/dosvm_audio_mixer.o" \
    "${guest_cpu[@]}" "$pool" -pthread -Wl,-u,pool_fixture_start -o "$out/dosvm_test"
CIUKI_VM_PROPERTIES=1 "$out/dosvm_test"
