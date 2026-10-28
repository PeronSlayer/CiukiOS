#!/usr/bin/env bash
# FDC register/DMA + real DOS VM frontend; no image build or guest run.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$root/build/host/floppy"
mkdir -p "$out"
exec 9>"$out/test.lock"
flock -n 9 || { echo 'Another floppy host test is running' >&2; exit 1; }
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1
san=address,undefined
fat_root="$root"
while [[ $# != 0 ]]; do
    case $1 in
        --ubsan) san=undefined; shift ;;
        --fat-source) fat_root="${2:?--fat-source requires a source root}"; shift 2 ;;
        *) echo 'usage: host_floppy_tests.sh [--ubsan] [--fat-source ROOT]' >&2; exit 2 ;;
    esac
done
flags=(-std=c17 -O1 -g -Wall -Wextra -Werror "-fsanitize=$san" -I "$root/src/kernel/include")
blocked=0
test_number=0
run_test() {
    test_number=$((test_number+1))
    local log="$out/run-$test_number.log" status=0
    "$@" >"$log" 2>&1 || status=$?
    cat "$log"
    if [[ $status != 0 ]]; then
        # Preserve leak detection and a non-green overall result. As in the
        # ACPICA runner, let independent fixtures finish when this sandbox
        # alone prevents LSan's process inspection at normal program exit.
        if rg -q 'LeakSanitizer has encountered a fatal error' "$log" &&
           ! rg -q 'ERROR: AddressSanitizer|runtime error:|Assertion|FAIL' "$log"; then
            echo "BLOCKED: LeakSanitizer process inspection (fixture $test_number)"
            blocked=$((blocked+1))
        else return "$status"; fi
    fi
}
python3 "$root/tests/host/floppy_compile_target.py"
clang "${flags[@]}" -DFS_HOST -D_POSIX_C_SOURCE=200809L \
    "$root/tests/host/floppy_test.c" "$root/src/kernel/drivers/floppy.c" \
    "$root/src/kernel/drivers/floppy_media.c" -o "$out/floppy_test"
run_test "$out/floppy_test"
python3 "$root/tests/host/vm/prepare_lifecycle.py" "$root" "$out"
for unit in vm_source opl3 mixer; do
    clang "${flags[@]}" -include "$root/tests/host/vm_audio_shim.h" \
        -c "$root/src/kernel/audio/$unit.c" -o "$out/$unit.o"
done
pool_args=()
if [[ $san == undefined ]]; then pool_args+=(--plain); fi
pool="$(python3 "$root/tests/host/vm/pool_build.py" "$root" "$out" "${pool_args[@]}")"
guest=()
if [[ -f /usr/include/x86emu.h ]]; then guest=(-DCIUKI_DOSVM_GUEST_CPU "$root/tests/host/dosvm_guest_cpu.c" -lx86emu); fi
clang "${flags[@]}" -DFS_HOST -D_POSIX_C_SOURCE=200809L \
    -c "$root/src/kernel/drivers/floppy.c" -o "$out/floppy-core.o"
clang "${flags[@]}" -DFS_HOST -D_POSIX_C_SOURCE=200809L \
    -c "$root/tests/host/floppy_vm_physical.c" -o "$out/floppy-physical.o"
clang "${flags[@]}" -DCIUKI_FDC_FIXTURE -Wno-pointer-to-int-cast -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$out" "$root/tests/host/dosvm_test.c" "$root/tests/host/dosvm_uaddr_model.c" \
    "$root/src/kernel/net/ipx_config.c" "$root/src/kernel/net/bridge.c" "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/vm/dosvm_ne2000.c" \
    "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" \
    "$out/vm_source.o" "$out/opl3.o" "$out/mixer.o" "$out/floppy-core.o" "$out/floppy-physical.o" "${guest[@]}" "$pool" \
    -pthread -Wl,-u,pool_fixture_start -o "$out/dosvm_test"
run_test env CIUKI_VM_FLOPPY=1 "$out/dosvm_test"
run_test python3 "$root/tests/host/floppy_fat_run.py" "$fat_root" "$san"
if [[ $blocked != 0 ]]; then echo "BLOCKED: $blocked LeakSanitizer inspections; no sanitizer override applied"; exit 1; fi
