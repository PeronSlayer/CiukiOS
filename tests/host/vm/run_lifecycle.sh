#!/usr/bin/env bash
# Focused production launch/lifecycle regression, no kernel or image build.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/vm/lifecycle"
mkdir -p "$out"
pool="$(python3 "$root/tests/host/vm/pool_build.py" "$root" "$out")"
python3 "$root/tests/host/vm/prepare_lifecycle.py" "$root" "$out"
flags=(-std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src/kernel/include")
for unit in vm_source opl3 mixer; do
    clang "${flags[@]}" -include "$root/tests/host/vm_audio_shim.h" \
        -c "$root/src/kernel/audio/$unit.c" -o "$out/audio_$unit.o"
done
clang "${flags[@]}" -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread \
    -Wno-pointer-to-int-cast -I "$out" "$root/tests/host/vm/lifecycle_test.c" \
    "$root/tests/host/dosvm_uaddr_model.c" \
    "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" \
    "$root/src/kernel/net/bridge.c" "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/vm/dosvm_ne2000.c" \
    "$out/audio_vm_source.o" "$out/audio_opl3.o" "$out/audio_mixer.o" \
    "$pool" -Wl,-u,pool_fixture_start -o "$out/lifecycle_test"
"$out/lifecycle_test"
