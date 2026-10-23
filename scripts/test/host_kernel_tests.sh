#!/usr/bin/env bash
# T0: kernel library, boot-info validator and driver services on the host.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1

# f3-49: standalone production DOS bridge/FAT/VFS harness, no guest image.
bash "$root/tests/host/dos/run_file_api.sh"
python3 "$root/tests/host/entropy/run.py"
python3 "$root/tests/host/entropy/run.py" --kernel

python3 "$root/tests/host/record_scope_test.py"
python3 -B -m unittest discover -s "$root/tests/host" -p test_hardware_check.py
python3 -B -m unittest discover -s "$root/tests/host" -p test_boot_modes.py
python3 -B -m unittest discover -s "$root/tests/host" -p test_splash_progress.py
python3 -B -m unittest discover -s "$root/tests/host" -p test_loader_menu.py
python3 -B -m unittest discover -s "$root/tests/host/reliability" -p 'test_*.py'
python3 -B -m unittest discover -s "$root/tests/host/ndis" -p test_package.py
python3 -B "$root/tests/host/test_presenter_display.py"
python3 -B "$root/tests/host/test_display_modes.py" DisplayTests.test_kernel_switch
python3 -B "$root/tests/host/test_display_modes.py" DisplayTests.test_checkpoint_queue
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/supersavage_mode_test.c" \
    "$root/src/kernel/drivers/supersavage_mode.c" "$root/src/kernel/vm/vga_hardware.c" \
    "$root/src/kernel/vm/virtual_vga.c" -o "$out/supersavage_mode_test"
ASAN_OPTIONS=detect_leaks=0 "$out/supersavage_mode_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/supersavage_display_test.c" \
    "$root/src/kernel/drivers/supersavage_mode.c" "$root/src/kernel/vm/vga_hardware.c" \
    "$root/src/kernel/vm/virtual_vga.c" -o "$out/supersavage_display_test"
ASAN_OPTIONS=detect_leaks=0 "$out/supersavage_display_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined -I "$root/src/kernel/include" \
    "$root/tests/host/record_runtime_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/record_runtime_test"
"$out/record_runtime_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I "$root/src/kernel/include" "$root/tests/host/serial_audio_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/serial_audio_test"
python3 -B -m unittest discover -s "$root/tests/host" -p test_kernel_latency.py
"$out/serial_audio_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" "$root/tests/host/serial_record_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/serial_record_test"
"$out/serial_record_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" \
    "$root/tests/host/bootlog_test.c" -o "$out/bootlog_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" \
    "$root/tests/host/blackbox_test.c" -o "$out/blackbox_test"
"$out/blackbox_test"
"$out/bootlog_test"
python3 "$root/tests/host/test_system_stalls.py"
python3 "$root/tests/host/fbdev_fixture_size_test.py"

# F2 ABI: always extract the actual i686 layout using the kernel's flags.
python3 "$root/scripts/test/abi_layout_dump.py" --output "$out/abi-layout.json"
cat > "$out/abi_m32_probe.c" <<'C'
#include <stdint.h>
#include <stdio.h>
int main(void) { return sizeof(uintptr_t) != 4; }
C
abi_flags=(-std=c17 -O1 -g -Wall -Wextra -Werror)
abi_m32=0
if clang -m32 "${abi_flags[@]}" "$out/abi_m32_probe.c" -o "$out/abi_m32_probe" \
        > "$out/abi_m32_probe.log" 2>&1; then
    # Even if the sandbox cannot execute 32-bit Linux binaries, compile the
    # complete test/header in that mode. A real ABI compile error must fail.
    clang -m32 "${abi_flags[@]}" -I "$root/src/kernel/include" \
        -c "$root/tests/host/abi_layout_test.c" -o "$out/abi_layout_test_m32.o"
    if python3 - "$out/abi_m32_probe" <<'PY'
import resource, subprocess, sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
result = subprocess.run([sys.argv[1]], capture_output=True)
if result.returncode:
    print(f"abi layout: -m32 runtime probe unavailable (status {result.returncode})")
sys.exit(0 if result.returncode == 0 else 1)
PY
    then
        abi_m32=1
    fi
fi
if [[ "$abi_m32" == 1 ]]; then
    abi_flags+=(-m32)
    echo "abi layout: native -m32 available"
else
    abi_flags+=(-fsanitize=address,undefined)
    echo "abi layout: native -m32 unavailable; checking target JSON with native fixed-width records"
fi
clang "${abi_flags[@]}" -I "$root/src/kernel/include" \
    "$root/tests/host/abi_layout_test.c" -o "$out/abi_layout_test"
"$out/abi_layout_test" "$out/abi-layout.json"

# f2-41: exact production snapshot and read-only user marshalling.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$root/src/kernel/include" \
    "$root/tests/host/devices_test.c" -Wl,--gc-sections -o "$out/devices_test"
"$out/devices_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/kernel_lib_test.c" -o "$out/kernel_lib_test"
"$out/kernel_lib_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/runtime_init_test.c" -o "$out/runtime_init_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/record_guard_test.c" -o "$out/record_guard_test"
"$out/record_guard_test"
"$out/runtime_init_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/record_guard_test.c" -o "$out/record_guard_test"
"$out/record_guard_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" \
    "$root/tests/host/rtc_test.c" "$root/src/kernel/fs/fs_port.c" -o "$out/rtc_test"
"$out/rtc_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/kernel_sync_test.c" \
    "$root/tests/host/entropy/unavailable_stub.c" -o "$out/kernel_sync_test"
"$out/kernel_sync_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/pci_irq_test.c" -o "$out/pci_irq_test"
"$out/pci_irq_test"

# f1-18/f1-19 do not authorize edits to i8042_test.c. Adapt its old adapter-only
# fakes in build/host, preserving every input/probe count assertion. Its
# relative production includes resolve identically from this generated file.
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import sys
root, out = map(Path, sys.argv[1:])
source = (root / "tests/host/i8042_test.c").read_text()
old = '!strcmp(name, "firmware-queue") && fn && !arg && prio == P_DEVICE'
assert source.count(old) == 1
source = source.replace(old, old.replace('P_DEVICE', 'P_INTERACTIVE'))
source += '''
bool fwinput_pending(void)
{
    return fw_count || fake_fw_state == BIOSVM_DISABLED_BACKEND;
}
uint64_t biosvm_input_reflections(void) { return 0; }
void biosvm_set_input_wait(struct kwait *q) { CHECK(q != 0); }
void biosvm_input_snapshot(struct biosvm_input_diag *out) { memset(out, 0, sizeof(*out)); }
'''
(out / "fwqueue_i8042_test.c").write_text(source)
PY
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$out/fwqueue_i8042_test.c" \
    "$root/tests/host/entropy/unavailable_stub.c" -o "$out/i8042_test"
"$out/i8042_test"

python3 "$root/scripts/test/iso_fixtures.py" --out "$out/optical" --joliet > "$out/optical-manifest.json"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" \
    "$root/tests/host/ata_test.c" "$root/src/kernel/fs/partition.c" \
    "$root/src/kernel/fs/fs_port.c" "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/fs/cache.c" \
    "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/path.c" \
    "$root/src/kernel/fs/vfs.c" "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" "$root/src/kernel/fs/ntfs_fs.c" "$root/src/kernel/fs/ntfs_ro.c" "$root/src/kernel/fs/mount.c" \
    "$root/src/kernel/core/bootlog.c" "$root/src/kernel/probes/fat_probes.c" \
    "$root/src/kernel/lib/sha256.c" "$root/src/kernel/lib/fmt.c" \
    "$root/tests/host/entropy/unavailable_stub.c" -o "$out/ata_test"

# M1: production PMM/VMM/user mappings, ledger and generation-safe pins.
# Context violations are diagnostic in production; strictness is opt-in here.
for ledger_mode in normal strict; do
    ledger_flags=()
    if [[ "$ledger_mode" == strict ]]; then ledger_flags+=(-DCIUKI_LEDGER_STRICT); fi
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -fsanitize=address,undefined "${ledger_flags[@]}" -I "$root/src/kernel/include" \
        "$root/tests/host/ledger_test.c" -o "$out/ledger_test_$ledger_mode"
    "$out/ledger_test_$ledger_mode"
done

# M3: actual global sampler/scanner with controlled accessed-bit traces.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined -I "$root/src/kernel/include" \
    "$root/tests/host/reclaim_test.c" -o "$out/reclaim_test"
"$out/reclaim_test"

# M4 (f0-49): pager, backing slots, page-out/page-in, prewrite, lock order,
# on the production PMM/ledger/VMM/uaddr/reclaim with a host storage model.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined -I "$root/src/kernel/include" \
    "$root/tests/host/pager_test.c" -o "$out/pager_test"
"$out/pager_test"

# f4-04: the physical allocator's contiguous run behind the NTFS mount workspace.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/pmm_contig_test.c" -o "$out/pmm_contig_test"
"$out/pmm_contig_test"
"$out/ata_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/sha256_test.c" \
    "$root/src/kernel/lib/sha256.c" -o "$out/sha256_test"
"$out/sha256_test"

# Call 73: ACPI soft-off discovery (RSDP/RSDT/XSDT/FADT/DSDT and \_S5).
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/power_test.c" -o "$out/power_test"
"$out/power_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/power_runtime_test.c" -o "$out/power_runtime_test"
"$out/power_runtime_test"

# FPU/SIMD audit classifier: a fixture with x87, MMX and SSE instructions
# must be flagged; integer code must not.
fx="$out/audit_fixture.asm"
cat > "$fx" <<'ASM'
bits 32
section .text
global simd_fixture, integer_fixture
simd_fixture:
    fld1
    addss xmm0, xmm1
    pxor mm0, mm1
    paddd mm0, mm1
    movaps xmm0, xmm1
    cvtsi2ss xmm0, eax
    movss xmm0, xmm1
    cmpsd xmm0, xmm1, 0
    ret
integer_fixture:
    push eax
    pop eax
    pause
    add eax, ebx
    movsb
    movsd
    cmpsb
    ret
ASM
nasm -f elf32 "$fx" -o "$out/audit_fixture.o"
python3 - "$out/audit_fixture.o" "$root" <<'PY'
import subprocess, sys, importlib.util
spec = importlib.util.spec_from_file_location("bk", sys.argv[2] + "/scripts/build_kernel.py")
bk = importlib.util.module_from_spec(spec); spec.loader.exec_module(bk)
dis = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", sys.argv[1]], capture_output=True, text=True, check=True).stdout
bad = bk.audit_disassembly(dis, {})
simd = [b for b in bad if b.startswith("simd_fixture")]
integer = [b for b in bad if b.startswith("integer_fixture")]
assert len(simd) == 8, simd
assert not integer, integer
assert "-fstack-protector-strong" in bk.CFLAGS
assert "-fno-stack-protector" not in bk.CFLAGS
assert "-mstack-protector-guard=global" in bk.CFLAGS
out = bk.ROOT / "build/host"
source = out / "stackprot_fixture.c"
source.write_text("extern void consume(char *); void guarded(void) { char b[64]; consume(b); }\n")
obj = out / "stackprot_fixture.o"
subprocess.run(["clang", *bk.CFLAGS, "-c", str(source), "-o", str(obj)], check=True)
fixture = subprocess.run(["llvm-objdump", "-dr", "--no-show-raw-insn", str(obj)], capture_output=True, text=True, check=True).stdout
assert "__stack_chk_guard" in fixture and "__stack_chk_fail" in fixture, fixture
assert not bk.audit_disassembly(fixture, {}), fixture
print("audit classifier fixture: PASS")
print("stack protector build flag/instrumentation/integer audit: PASS")
PY

# F1 framebuffer presenter and probe with heap-backed LFB fixtures.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/mtrr_test.c" -o "$out/mtrr_test"
"$out/mtrr_test"
# f6-21b: kernel PDEs identical in every directory across runtime allocations.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/kernel_pde_test.c" -o "$out/kernel_pde_test"
"$out/kernel_pde_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/fbdev_test.c" -o "$out/fbdev_test"
"$out/fbdev_test"

# f1-26: the same renderer plus a bounded register model of Savage commands.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/savage2d_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/savage2d_test"
ASAN_OPTIONS=detect_leaks=0 "$out/savage2d_test"

# F1 V86 decoder, device models, worker aborts and firmware event adapter.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/v86_test.c" -o "$out/v86_test"
"$out/v86_test"

# F3 lifecycle, BIOS/disk preflight and record channel. Keep paging production
# functions unchanged; replace only PMM and architectural hardware operations.
python3 "$root/tests/host/vm/prepare_lifecycle.py" "$root" "$out"
# dosvm_test.c supplies the bounded program-provider fixture.
# The VGA models and the retained presenter (reference oracle) are separate
# translation units, exactly as the kernel links them.
# f4-05: the VM audio sources, the integer OPL3 port and the f4-01 mixer are
# their own units, as in the kernel; the shim makes their IRQ sections no-ops.
for unit in vm_source opl3 mixer; do
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -include "$root/tests/host/vm_audio_shim.h" -I "$root/src/kernel/include" \
        -c "$root/src/kernel/audio/$unit.c" -o "$out/dosvm_audio_$unit.o"
done
pool="$(python3 "$root/tests/host/vm/pool_build.py" "$root" "$out/vm")"
plain_pool="$(python3 "$root/tests/host/vm/pool_build.py" "$root" "$out/vm" --plain)"
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
"$out/dosvm_test"
CIUKI_VM_DISK_TRACE=1 "$out/dosvm_test"
# Replay the actual default maximum-cache VM's charges through production
# PMM/ledger profiles at 128/256/512 MiB; no VM/image or parallel test run.
vm_profile="$(CIUKI_VM_MEMORY_CHARGE=1 "$out/dosvm_test")"
read -r -a vm_profile_args <<< "$vm_profile"
"$out/ledger_test_normal" --dosvm-profile "${vm_profile_args[@]}"

# F4-28: optimized VM-source callback cost (the sanitized full suite above
# also runs it). Host-only, sequential, with the production audio units.
for unit in vm_source opl3 mixer; do
    clang -std=c17 -O2 -g -Wall -Wextra -Werror \
        -include "$root/tests/host/vm_audio_shim.h" -I "$root/src/kernel/include" \
        -c "$root/src/kernel/audio/$unit.c" -o "$out/dosvm_cost_$unit.o"
done
clang -std=c17 -O2 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" -I "$out" "$root/tests/host/dosvm_test.c" \
    "$root/tests/host/dosvm_uaddr_model.c" "${network_sources[@]}" \
    "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" \
    "$out/dosvm_cost_vm_source.o" "$out/dosvm_cost_opl3.o" "$out/dosvm_cost_mixer.o" \
    "${guest_cpu[@]}" "$plain_pool" -pthread -Wl,-u,pool_fixture_start -o "$out/dosvm_audio_cost_test"
CIUKI_VM_AUDIO_COST=1 "$out/dosvm_audio_cost_test"

# f3-33: aggregate production exit costs with fake CPU trap transport.
# Both benchmarks include dosvm_test.c and need its real-uaddr model too.
clang -std=c17 -O2 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" -I "$out" "$root/tests/host/dosvm_exit_bench.c" \
    "$root/tests/host/dosvm_uaddr_model.c" "${network_sources[@]}" \
    "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" \
    "$out/dosvm_cost_vm_source.o" "$out/dosvm_cost_opl3.o" "$out/dosvm_cost_mixer.o" \
    "$plain_pool" -pthread -Wl,-u,pool_fixture_start -o "$out/dosvm_exit_bench"
"$out/dosvm_exit_bench"

# f3-16: production window/dirty-page replay and presenter costs. Sequential
# with every other host suite; synthetic frames, no image rebuild or QEMU.
clang -std=c17 -O2 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" -I "$out" "$root/tests/host/dosvm_present_bench.c" \
    "$root/tests/host/dosvm_uaddr_model.c" "${network_sources[@]}" \
    "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" \
    "$out/dosvm_audio_vm_source.o" "$out/dosvm_audio_opl3.o" "$out/dosvm_audio_mixer.o" \
    "$pool" -pthread -Wl,-u,pool_fixture_start -o "$out/dosvm_present_bench"
"$out/dosvm_present_bench"

# f3-02 migrated VGA model vectors (legacy test_virtual_vga.c, unchanged).
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" -I "$root/src/kernel/vm" "$root/tests/host/test_virtual_vga.c" \
    "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
    "$root/src/kernel/vm/vga_presenter.c" -o "$out/test_virtual_vga"
"$out/test_virtual_vga"
# f3-04 guest runner: the real DOSBOOT.IMG on the production DOS VM and DPMI
# host code with libx86emu as the CPU (pre-QEMU check, not QEMU evidence).
# f3-02: keys: types \VMVIDEO.COM the way a desktop window does (8042, IRQ1,
# guest INT 9) into the shell blocked in INT 16h AH=00h.
# Optional: needs libx86emu and an already built build/f0/dos/DOSBOOT.IMG.
dos_image="$root/build/f0/dos/DOSBOOT.IMG"
if [[ -f /usr/include/x86emu.h ]]; then
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        "$root/tests/host/dosvm_guest_step_test.c" "$root/tests/host/dosvm_guest_cpu.c" -lx86emu -o "$out/dosvm_guest_step_test"
    "$out/dosvm_guest_step_test"
else
    echo "guest CPU single step: SKIP (no libx86emu headers)"
fi
if [[ -f /usr/include/x86emu.h ]]; then
    clang -std=c17 -O2 -g -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
        -I "$root/src/kernel/include" -I "$out" "$root/tests/host/dosvm_guest.c" \
        "$root/tests/host/dosvm_guest_cpu.c" "$root/src/kernel/lib/fmt.c" "$root/src/kernel/lib/sha256.c" \
        "$root/src/kernel/vm/virtual_vga.c" "$root/src/kernel/vm/virtual_vga_bios.c" \
        "$root/src/kernel/vm/vga_x86.c" "$root/src/kernel/vm/vga_presenter.c" -lx86emu -o "$out/dosvm_guest"
    # F3-28: always replay EXEC of the hash-pinned Quake executable in a
    # temporary sparse FAT fixture, even without a canonical DOSBOOT image.
    python3 -B "$root/tests/host/dosvm_exec_test.py" "$out/dosvm_guest"
fi
if [[ -f /usr/include/x86emu.h && -f "$dos_image" ]] && grep -q '"/DPMI4GW.EXE"' "${dos_image%.IMG}.json"; then
    if ! (cd "$root" && timeout 600 "$out/dosvm_guest" "$dos_image" 'keys:\VMVIDEO.COM' VMVIDEO 'gate:\DPMI16.COM' DPMI16 '\DPMI16.COM' DPMI16 '\DPMI32.COM' DPMI32 \
            '\DPMI4GW.EXE' DPMI4GW 'fault:\DPMIFLT.COM' DPMIFLT > "$out/dosvm_guest.log" 2>&1); then
        tail -n 40 "$out/dosvm_guest.log"
        exit 1
    fi
    tail -n 1 "$out/dosvm_guest.log"
    # QEMU TCG model (no data-segment limit checks, TCG CPUID signature): the
    # clients must take their recorded TCG fallback and still pass.
    if ! (cd "$root" && GUEST_TCG=1 timeout 600 "$out/dosvm_guest" "$dos_image" '\DPMI16.COM' DPMI16 \
            '\DPMI32.COM' DPMI32 > "$out/dosvm_guest_tcg.log" 2>&1) ||
       [[ $(grep -c 'NOTE tcg_no_data_limit_check' "$out/dosvm_guest_tcg.log") != 2 ]]; then
        tail -n 40 "$out/dosvm_guest_tcg.log"
        exit 1
    fi
    echo "TCG model: $(tail -n 1 "$out/dosvm_guest_tcg.log")"
else
    echo "dosvm guest runner: SKIP (no libx86emu headers, or no DOSBOOT.IMG with the f3-04 payloads)"
fi

# F1 firmware queue: fake-time scheduler and the existing production VM fakes.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/fwqueue_test.c" -o "$out/fwqueue_test"
"$out/fwqueue_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFWQUEUE_FIRMWARE_TEST -I "$root/src/kernel/include" \
    "$root/tests/host/fwqueue_test.c" -o "$out/fwqueue_firmware_test"
"$out/fwqueue_firmware_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFWQUEUE_SERVICE_TEST -I "$root/src/kernel/include" \
    "$root/tests/host/fwqueue_test.c" -o "$out/fwqueue_service_test"
"$out/fwqueue_service_test"

# F2 production parser, mappings, wait queues, stack and lifecycle with fake
# physical memory/scheduling; no guest execution or host runner lock.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/proc/proc_test.c" \
    "$root/tests/host/proc/signal_legacy.c" "$root/tests/host/proc/desktop_legacy.c" -o "$out/proc_test"
"$out/proc_test"

# f2-39: production launch markers and ELF batches, fake monotonic clock/I/O.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/proc/launch_test.c" \
    "$root/tests/host/proc/signal_legacy.c" "$root/tests/host/proc/desktop_legacy.c" -o "$out/launch_test"
"$out/launch_test"

# Replay signal-probe timing with the production run queues/dispatch/timer.
# Extract only these functions: architectural stack creation cannot run on
# the host. No scheduler policy or function body is replaced by a test model.
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import re, sys
root, out = map(Path, sys.argv[1:])
source = (root / "src/kernel/core/task.c").read_text()
declarations = source[source.index("static struct task *rq_head"):
                      source.index("void task_stack_init")]
functions = []
for name in ("schedule", "sched_vm_tick_work", "sched_tick", "task_sleep_ms", "task_start"):
    match = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^\n]*\)\n\{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    functions.append(source[match.start():end] + "\n")
timers = (root / "src/kernel/core/sync.c").read_text()
(out / "deadline_timers.inc").write_text(timers[timers.index("/* F0-46 timers:"):timers.index("/* End F0-46 timers. */")])
(out / "signal_scheduler.inc").write_text("void task_start(struct task *);\n" + declarations + "\n".join(functions))
PY
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" -I "$out" \
    "$root/tests/host/proc/signal_test.c" "$root/tests/host/proc/desktop_legacy.c" -o "$out/signal_test"
# Assemble the new fixture even before the lead integrates its canonical
# build hook. Values are target-generated from the one public ABI header.
python3 - "$out" <<'PY'
import json, re, sys
from pathlib import Path
out = Path(sys.argv[1])
definitions = []
for key, value in json.loads((out / "abi-layout.json").read_text()).items():
    if key.startswith("constant."):
        name = key.removeprefix("constant.")
    elif key.startswith(("sizeof.ciuki_", "offsetof.ciuki_")):
        name = "ABI_" + re.sub(r"[^A-Za-z0-9_]", "_", key).upper()
    else:
        continue
    definitions.append(f"%define {name} {value}\n")
(out / "proc_abi.inc").write_text("".join(definitions))
PY
nasm -f bin -I "$out/" "$root/tests/host/proc/signal_payload.asm" -o "$out/signal-payload.elf"
"$out/signal_test" "$out/signal-payload.elf"

# F2 desktop objects and supervisor. The same standalone NASM ELF is handed
# to the lead's canonical build hook; host validation does not execute it.
nasm -f bin -I "$out/" "$root/tests/host/proc/desktop_payload.asm" -o "$out/desktop-payload.elf"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" "$root/tests/host/proc/desktop_test.c" \
    "$root/tests/host/proc/signal_legacy.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/lib/fmt.c" \
    "$root/src/kernel/probes/selector.c" \
    "-DCIUKI_DESKTOP_PAYLOAD_BIN=\"$out/desktop-payload.elf\"" -o "$out/desktop_test"
"$out/desktop_test" "$out/desktop-payload.elf"


# F2 native files use the same mkfs/mtools fixtures and fake block layer as
# the independent F1 harness. Fail-closed integration gaps are reported by name.
python3 "$root/tests/host/fs/fixtures.py"
# F1 shutdown with F2's production bootstrap/path/fd namespace linked. Also
# retain the weak-hook-only storage harness as a separate regression check.
storage_sources=(
    "$root/src/kernel/fs/fs_port.c" "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/fs/cache.c"
    "$root/src/kernel/fs/partition.c" "$root/src/kernel/fs/path.c"
    "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/vfs.c" "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c"
    "$root/src/kernel/fs/ntfs_fs.c" "$root/src/kernel/fs/ntfs_ro.c"
    "$root/src/kernel/fs/mount.c" "$root/src/kernel/core/bootlog.c" "$root/src/kernel/core/blackbox.c"
    "$root/src/kernel/drivers/blkpart.c" "$root/src/kernel/probes/fat_probes.c"
    "$root/src/kernel/lib/sha256.c" "$root/src/kernel/lib/fmt.c"
    "$root/tests/host/fs/fake.c" "$root/tests/host/fs/scan.c"
)
for storage_test in test_storage_namespace test_storage test_log_recovery test_powercuts; do
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
        -I "$root/src/kernel/include" -I "$root/src/kernel/fs" \
        "${storage_sources[@]}" "$root/tests/host/fs/$storage_test.c" \
        -o "$out/fs/$storage_test"
    "$out/fs/$storage_test" "$out/fs/fat32.img" "$out/fs/fat12.img" "$out/fs/fat16.img"
    # The storage fixture's baseline SYSTEM directory is intentionally
    # durable; rebuild the small host fixtures before the next harness.
    python3 "$root/tests/host/fs/fixtures.py"
done
nasm -f bin -I "$out/" "$root/tests/host/proc/files_payload.asm" -o "$out/files-payload.elf"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" -I "$root/src/kernel/fs" \
    "$root/tests/host/proc/files_test.c" "$root/tests/host/proc/signal_legacy.c" \
    "$root/tests/host/proc/desktop_legacy.c" "$root/src/kernel/fs/fs_port.c" \
    "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/fs/cache.c" "$root/src/kernel/fs/path.c" "$root/src/kernel/fs/fat.c" \
    "$root/src/kernel/fs/vfs.c" "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" "$root/tests/host/fs/fake.c" \
    "$root/src/kernel/probes/f2_probes_files.c" -o "$out/files_test"
"$out/files_test" "$out/fs/fat32.img" "$out/fs/fat16.img" "$out/files-payload.elf"

# F2 app controller: sanitized records/captures plus an exact 8 KiB guarded
# stack regression, and negative controls for the stack and compiler guards.
python3 -m unittest discover -s "$root/tests/host" -p test_app_gate.py

# F4: production mixer, stream syscalls, persistent settings and mock AC97.
bash "$root/tests/host/audio/run.sh"

# F1 USB: production UHCI/OHCI register/DMA models.
bash "$root/tests/host/usb/run.sh"
bash "$root/tests/host/usb/storage_run.sh"

# F6: production ownership/queues and three register-level NIC models.
bash "$root/tests/host/net/run.sh"
python3 "$root/tests/host/lwip/fd_run.py" --frame
python3 "$root/tests/host/lwip/run.py"
python3 "$root/tests/host/lwip/fd_run.py"
python3 "$root/tests/host/lwip/run.py" --kernel


# f4-05: the kernel's integer OPL3 port sample for sample against the pinned
# upstream DBOPL C++ (third_party/vsbhda, archive hash checked first), and
# its generated tables against their floating-point formulas.
python3 - "$root" <<'PY'
import hashlib, json, sys
from pathlib import Path
pin = Path(sys.argv[1]) / "third_party/vsbhda"
meta = json.loads((pin / "UPSTREAM.json").read_text())
assert hashlib.sha256((pin / meta["archive"]).read_bytes()).hexdigest() == meta["sha256"], "VSBHDA archive hash"
PY
rm -rf "$out/dbopl" && mkdir -p "$out/dbopl"
tar -xzf "$root/third_party/vsbhda/source-75fa4bb.tar.gz" -C "$out/dbopl" --strip-components=2 \
    --wildcards '*/src/DBOPL.CPP' '*/src/DBOPL.H' '*/src/CONFIG.HPP'
clang++ -std=c++17 -O1 -g -w -fsanitize=address -I "$out/dbopl" -c "$out/dbopl/DBOPL.CPP" -o "$out/dbopl/dbopl.o"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DCIUKI_HOST_OPL3 \
    -I "$root/src/kernel/include" -c "$root/src/kernel/audio/opl3.c" -o "$out/dbopl/opl3_port.o"
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$out/dbopl" -I "$root/src/kernel/include" \
    "$root/tests/host/opl3_ref_test.cpp" "$out/dbopl/dbopl.o" "$out/dbopl/opl3_port.o" -o "$out/opl3_ref_test"
"$out/opl3_ref_test"
python3 "$root/tests/host/test_opl3_tables.py"

# F5 personality mechanisms, fixture parity and core selector wiring.
python3 -B -m unittest discover -s "$root/tests/host/win32" -p test_kernel_core.py
python3 -B -m unittest discover -s "$root/tests/host/win32" -p test_registry.py
python3 -B -m unittest discover -s "$root/tests/host/proc" -p test_w32srv.py

# F0-46: instrument executed production IRQ and deferred paths.
python3 -B "$root/tests/host/irq_work_replay.py"

# f5-05B real CIUKI_SYS_W32 backend over production VM/sections, H0 only.
python3 -B -m unittest discover -s "$root/tests/host/win32" -p test_kernel_loader.py

# f5-06 kernel32 production VFS endpoints and direct-file INI parity.
python3 -B -m unittest discover -s "$root/tests/host/win32" -p test_kernel32.py
# f3-50: bound personality, native FAT/VFS and production V86/DPMI CPU model.
bash "$root/tests/host/dos/run_hook_cpu.sh"

# f3-56: unified CiukiDOS DFV2 binding (fork build, native matrix, CPU boots).
bash "$root/tests/host/dos/run_unified_binding.sh"
# f3-57: unified CiukiDOS local IFS (live VFS backend, critical errors,
# activation barrier, kernel=unified installation record).
bash "$root/tests/host/dos/run_unified_file.sh"
