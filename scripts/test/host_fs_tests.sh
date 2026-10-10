#!/usr/bin/env bash
# Host-only production FAT/VFS qualification; no boot image or QEMU.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/host/fs
exec 9>build/host/fs/test.lock
flock -n 9 || { echo 'Another host filesystem test is running' >&2; exit 1; }
export TMPDIR="$PWD/build/host/fs"
export PYTHONDONTWRITEBYTECODE=1
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
# LSan needs ptrace, unavailable in the implementer's sandbox. ASan and UBSan
# are mandatory; leak detection is explicitly not claimed by this runner.
printf '%s\n' 'SANITIZERS ASan=on UBSan=on LSan=off (sandbox ptrace restriction)'
missing=0
for tool in clang python3 mkfs.fat fsck.fat mcopy mmd mdir mtype nm; do
    if ! command -v "$tool" >/dev/null; then printf 'MISSING TOOL: %s\n' "$tool"; missing=1; fi
done
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c \
    tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_fs.c \
    -o build/host/fs/test_fs
printf '%s\n' 'PASS host compile: clang C17 ASan/UBSan -Wall -Wextra -Werror'
python3 tests/host/fs/kernel_compile.py
if ((missing)); then echo 'Fixture tests NOT RUN: required tool missing'; exit 1; fi
python3 tests/host/fs/fixtures.py
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/fs -Isrc/kernel/include \
    src/kernel/fs/fs_port.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/mount.c \
    src/kernel/core/bootlog.c src/kernel/drivers/blkpart.c src/kernel/probes/fat_probes.c \
    src/kernel/lib/sha256.c src/kernel/lib/fmt.c tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_storage.c \
    -o build/host/fs/test_storage
cp --sparse=always build/host/fs/fat32.img build/host/fs/storage-work.img
build/host/fs/test_storage build/host/fs/storage-work.img build/host/fs/fat12.img build/host/fs/fat16.img
fsck.fat -n build/host/fs/storage-work.img
rm build/host/fs/storage-work.img
: > build/host/fs/crash-fsck.log
build/host/fs/test_fs cache build/host/fs/fat16.img
build/host/fs/test_fs partition build/host/fs/fat16.img
for bits in 12 16 32; do
    image="build/host/fs/fat${bits}.img"
    build/host/fs/test_fs read "$image"
    build/host/fs/test_fs fault "$image"
    build/host/fs/test_fs crash "$image"
    cp --sparse=always "$image" build/host/fs/work.img
    build/host/fs/test_fs write build/host/fs/work.img
    fsck.fat -n build/host/fs/work.img
    python3 tests/host/fs/verify.py build/host/fs/work.img
    rm build/host/fs/work.img
done
python3 tests/host/fs/classify_crashes.py
python3 - <<'PY'
from pathlib import Path
root=Path('build/host/fs')
files=[p for p in root.rglob('*') if p.is_file()]
logical=sum(p.stat().st_size for p in files)
allocated=sum(p.stat().st_blocks*512 for p in files)
assert logical<200_000_000 and allocated<200_000_000,(logical,allocated)
print(f'ARTIFACTS files={len(files)} logical_bytes={logical} allocated_bytes={allocated} limit=200000000')
print('PASS host filesystem suite: 14 groups, 3 fsck checks, 3 independent mtools comparisons')
PY
