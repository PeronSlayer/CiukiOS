#!/usr/bin/env bash
# T0: kernel library, boot-info validator and driver services on the host.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1

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

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/kernel_lib_test.c" -o "$out/kernel_lib_test"
"$out/kernel_lib_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/kernel_sync_test.c" -o "$out/kernel_sync_test"
"$out/kernel_sync_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/i8042_test.c" -o "$out/i8042_test"
"$out/i8042_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -pthread -I "$root/src/kernel/include" \
    "$root/tests/host/ata_test.c" "$root/src/kernel/fs/partition.c" \
    "$root/src/kernel/lib/sha256.c" -o "$out/ata_test"
"$out/ata_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/sha256_test.c" \
    "$root/src/kernel/lib/sha256.c" -o "$out/sha256_test"
"$out/sha256_test"

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
print("audit classifier fixture: PASS")
PY

# F1 framebuffer presenter and probe with heap-backed LFB fixtures.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/fbdev_test.c" -o "$out/fbdev_test"
"$out/fbdev_test"

# F2 production parser, mappings, wait queues, stack and lifecycle with fake
# physical memory/scheduling; no guest execution or host runner lock.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/proc/proc_test.c" -o "$out/proc_test"
"$out/proc_test"
