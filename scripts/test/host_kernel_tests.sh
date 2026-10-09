#!/usr/bin/env bash
# T0: kernel library, boot-info validator and driver services on the host.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1
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
