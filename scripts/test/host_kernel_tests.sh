#!/usr/bin/env bash
# T0: kernel library, boot-info validator and driver services on the host.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1

python3 "$root/tests/host/record_scope_test.py"

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
    -I "$root/src/kernel/include" "$root/tests/host/kernel_sync_test.c" -o "$out/kernel_sync_test"
"$out/kernel_sync_test"

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
    -I "$root/src/kernel/include" "$out/fwqueue_i8042_test.c" -o "$out/i8042_test"
"$out/i8042_test"

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" \
    "$root/tests/host/ata_test.c" "$root/src/kernel/fs/partition.c" \
    "$root/src/kernel/fs/fs_port.c" "$root/src/kernel/fs/cache.c" \
    "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/path.c" \
    "$root/src/kernel/fs/vfs.c" "$root/src/kernel/fs/mount.c" \
    "$root/src/kernel/core/bootlog.c" "$root/src/kernel/probes/fat_probes.c" \
    "$root/src/kernel/lib/sha256.c" "$root/src/kernel/lib/fmt.c" -o "$out/ata_test"
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
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/fbdev_test.c" -o "$out/fbdev_test"
"$out/fbdev_test"

# F1 V86 decoder, device models, worker aborts and firmware event adapter.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/v86_test.c" -o "$out/v86_test"
"$out/v86_test"

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

clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/src/kernel/include" "$root/tests/host/proc/signal_test.c" "$root/tests/host/proc/desktop_legacy.c" -o "$out/signal_test"
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
    -I "$root/src/kernel/include" "$root/tests/host/proc/desktop_test.c" \
    "$root/tests/host/proc/signal_legacy.c" "$root/src/kernel/lib/sha256.c" -o "$out/desktop_test"
"$out/desktop_test" "$out/desktop-payload.elf"

# F2 native files use the same mkfs/mtools fixtures and fake block layer as
# the independent F1 harness. Fail-closed integration gaps are reported by name.
python3 "$root/tests/host/fs/fixtures.py"
nasm -f bin -I "$out/" "$root/tests/host/proc/files_payload.asm" -o "$out/files-payload.elf"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/include" -I "$root/src/kernel/fs" \
    "$root/tests/host/proc/files_test.c" "$root/tests/host/proc/signal_legacy.c" \
    "$root/tests/host/proc/desktop_legacy.c" "$root/src/kernel/fs/fs_port.c" \
    "$root/src/kernel/fs/cache.c" "$root/src/kernel/fs/path.c" "$root/src/kernel/fs/fat.c" \
    "$root/src/kernel/fs/vfs.c" "$root/tests/host/fs/fake.c" -o "$out/files_test"
"$out/files_test" "$out/fs/fat32.img" "$out/fs/fat16.img" "$out/files-payload.elf"
