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
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_fs.c \
    -o build/host/fs/test_fs
printf '%s\n' 'PASS host compile: clang C17 ASan/UBSan -Wall -Wextra -Werror'
# F1-83: the same bounded request engine used by cache and ATA/USB adapters.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/blkreq.c \
    tests/host/fs/test_blkreq.c -o build/host/fs/test_blkreq
build/host/fs/test_blkreq
# F1-86: exact reservation holder/waiter fields across a slow real FAT write.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    tests/host/fs/test_vfs_contention.c -Wl,--wrap=fs_lock_acquired,--wrap=fs_lock_released -o build/host/fs/test_vfs_contention
build/host/fs/test_vfs_contention
# f1-87: T23 request mix and 111 ms device commands. The strict amendment-1
# gate also requires bounded logical holds; diagnostics alone cannot pass B.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    tests/host/fs/test_t23_holds.c -Wl,--wrap=fs_lock_acquired,--wrap=fs_lock_released -o build/host/fs/test_t23_holds
if [[ ${CIUKI_F187_REQUIRE_BOUNDED:-0} == 1 ]]; then
    build/host/fs/test_t23_holds --require-bounded
else
    build/host/fs/test_t23_holds
fi
rm build/host/fs/test_t23_holds
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs tests/host/fs/test_blkreq_timer.c \
    -o build/host/fs/test_blkreq_timer
build/host/fs/test_blkreq_timer
# f4-04: cache page layouts per device sector size (mixed 512/2048-byte devices).
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    tests/host/fs/test_cache.c -o build/host/fs/test_cache
build/host/fs/test_cache
# F1-82: deterministic dirty-budget/reservation/error cases, then the timed
# 10 MiB production FAT/VFS copy versus a 16 KiB / 20 ms reader.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    tests/host/fs/test_cache_writeback.c -o build/host/fs/test_cache_writeback
build/host/fs/test_cache_writeback
clang -DFS_HOST -DF182_AFTER -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    tests/host/fs/test_writeback.c -Wl,--wrap=cache_read_ahead,--wrap=cache_write -o build/host/fs/test_writeback
build/host/fs/test_writeback
if ((missing)); then echo 'Fixture tests NOT RUN: required tool missing'; exit 1; fi
python3 tests/host/fs/fixtures.py
# f0-49: production pagefile adapter: slot map oracle, in-place growth and
# shrink, exclusivity, reserved records, I/O failures and power cuts.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c src/kernel/fs/pagefile.c \
    tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_pagefile.c -o build/host/fs/test_pagefile
rm -f build/host/fs/pagefile32.img
truncate -s 288M build/host/fs/pagefile32.img
mkfs.fat --invariant -F 32 -S 512 -s 8 -f 2 -i 00C1A049 build/host/fs/pagefile32.img > /dev/null
MTOOLSRC=build/host/fs/mtoolsrc mcopy -i build/host/fs/pagefile32.img build/host/fs/seed.bin ::/SEED.BIN
cp --sparse=always build/host/fs/fat32.img build/host/fs/pagefile-small.img
cp --sparse=always build/host/fs/fat16.img build/host/fs/pagefile-full.img
build/host/fs/test_pagefile build/host/fs/pagefile32.img build/host/fs/pagefile-small.img build/host/fs/pagefile-full.img
fsck.fat -n build/host/fs/pagefile32.img > /dev/null
MTOOLSRC=build/host/fs/mtoolsrc mcopy -n -i build/host/fs/pagefile32.img ::/SEED.BIN build/host/fs/pagefile-seed.bin
cmp build/host/fs/seed.bin build/host/fs/pagefile-seed.bin
rm -f build/host/fs/pagefile32.img build/host/fs/pagefile-small.img build/host/fs/pagefile-full.img build/host/fs/pagefile-seed.bin
# f3-51c: per-open-file FAT cluster cursor (linear sequential read of a
# 27,336,704-byte games disk on FAT32 with 4 KiB clusters, as C:, plus the
# invalidation rules) and the VFS write generations of the launch digest cache.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    src/kernel/lib/sha256.c tests/host/fs/fake.c tests/host/fs/test_cluster_cursor.c \
    -Wl,--wrap=cache_read_ahead -o build/host/fs/test_cluster_cursor
rm -f build/host/fs/cursor32.img build/host/fs/cursor-game.bin
truncate -s 288M build/host/fs/cursor32.img
mkfs.fat --invariant -F 32 -S 512 -s 8 -f 2 -i 00C1A51C build/host/fs/cursor32.img > /dev/null
cursor_sha="$(python3 - build/host/fs/cursor-game.bin <<'PY'
import hashlib, sys
rows = [bytes((b + j * 7) & 255 for j in range(512)) for b in range(256)]
data = b''.join(rows[(s * 131 + (s >> 8)) & 255] for s in range(27336704 // 512))
open(sys.argv[1], 'wb').write(data)
print(hashlib.sha256(data).hexdigest())
PY
)"
MTOOLSRC=build/host/fs/mtoolsrc mmd -i build/host/fs/cursor32.img ::/GAMES
MTOOLSRC=build/host/fs/mtoolsrc mcopy -i build/host/fs/cursor32.img build/host/fs/cursor-game.bin ::/GAMES/CIUKIGAM.IMG
rm -f build/host/fs/cursor-game.bin
build/host/fs/test_cluster_cursor build/host/fs/cursor32.img "$cursor_sha"
fsck.fat -n build/host/fs/cursor32.img > /dev/null
rm -f build/host/fs/cursor32.img
# w32srv registry directory commit (apps/w32srv/directory.c over posix_io.c)
# replayed on the production FAT32/VFS with the guest's 4 KiB clusters and an
# mtools-made C:\WINDOWS; first with the F1 write gate closed (negative
# control: EROFS at open, the w32-dialogs symptom on image ec9f5854), then
# open. The slots it leaves are read back independently with mtools.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    tests/host/fs/fake.c tests/host/fs/test_registry_commit.c -o build/host/fs/test_registry_commit
rm -rf build/host/fs/registry32.img build/host/fs/registry-expect build/host/fs/registry-seed-*.bin build/host/fs/registry-mtools-*.bin
truncate -s 288M build/host/fs/registry32.img
mkfs.fat --invariant -F 32 -S 512 -s 8 -f 2 -i 00C1A0E9 build/host/fs/registry32.img > /dev/null
python3 - build/host/fs/registry-seed-system.bin build/host/fs/registry-seed-user.bin <<'PY'
import sys
for path, size, salt in ((sys.argv[1], 6381, 0), (sys.argv[2], 3797, 1)):
    open(path, 'wb').write(b'CRG1' + bytes((i * 13 + salt) & 255 for i in range(size - 4)))
PY
MTOOLSRC=build/host/fs/mtoolsrc mmd -i build/host/fs/registry32.img ::/WINDOWS ::/WINDOWS/SYSTEM ::/WINDOWS/CRASH
MTOOLSRC=build/host/fs/mtoolsrc mcopy -i build/host/fs/registry32.img build/host/fs/registry-seed-system.bin ::/WINDOWS/SYSTEM.CRG.0
MTOOLSRC=build/host/fs/mtoolsrc mcopy -i build/host/fs/registry32.img build/host/fs/registry-seed-user.bin ::/WINDOWS/USER.CRG.0
mkdir -p build/host/fs/registry-expect
build/host/fs/test_registry_commit build/host/fs/registry32.img build/host/fs/registry-expect
fsck.fat -n build/host/fs/registry32.img > /dev/null
for slot in 0 1; do
    MTOOLSRC=build/host/fs/mtoolsrc mcopy -n -i build/host/fs/registry32.img "::/WINDOWS/SERVICE.CRS.$slot" "build/host/fs/registry-mtools-$slot.bin"
    cmp "build/host/fs/registry-expect/SERVICE.CRS.$slot" "build/host/fs/registry-mtools-$slot.bin"
done
printf '%s\n' 'PASS registry-commit mtools readback: SERVICE.CRS.0/1 byte-exact, fsck clean'
rm -rf build/host/fs/registry32.img build/host/fs/registry-expect build/host/fs/registry-seed-*.bin build/host/fs/registry-mtools-*.bin
# F1-85: actual controller, ledger profiles, stream priority/cancellation, FAT EOF.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    tests/host/fs/fake.c tests/host/fs/test_cache_adaptive.c -o build/host/fs/test_cache_adaptive
build/host/fs/test_cache_adaptive build/host/fs/fat16.img
# F1-84: common file/page identities and all native/Win32/DOS lock pairs.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_publication.c -o build/host/fs/test_publication
for bits in 12 16 32; do
    build/host/fs/test_publication "build/host/fs/fat${bits}.img"
done
# C3 replay, including the former same-volume ticket as negative control.
clang -DFS_HOST -DF182_AFTER -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    -DCIUKI_USB_HOST -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tests/host/fs/test_fairness.c tests/host/usb/f1_83_source.c \
    src/kernel/usb/usb_core.c src/kernel/usb/usb_storage.c src/kernel/usb/uhci.c src/kernel/usb/usb_interrupt.c \
    src/kernel/lib/fmt.c src/kernel/core/bootlog.c src/kernel/drivers/blkpart.c src/kernel/fs/mount.c \
    -Wl,--wrap=cache_read_ahead,--wrap=cache_write -o build/host/fs/test_fairness
build/host/fs/test_fairness
# Sparse virtual layouts exercise capacity without creating multi-GiB images.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/fat.c src/kernel/fs/path.c tests/host/fs/test_mount_budget.c \
    -o build/host/fs/test_mount_budget
build/host/fs/test_mount_budget
# f1-39: production coalescing, shared hits and request accounting.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/fat.c src/kernel/fs/path.c tests/host/fs/test_throughput.c \
    -o build/host/fs/test_throughput
build/host/fs/test_throughput
# F1-44: repeat that exact FAT/cache workload through production ATA DMA.
python3 scripts/test/iso_fixtures.py --out build/host/optical --joliet > build/host/optical-manifest.json
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -Dmain=ata_fixture_main -std=c17 \
    -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -pthread \
    -Isrc/kernel/include -c tests/host/ata_test.c -o build/host/fs/ata_dma_fixture.o
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -DATA_DMA_THROUGHPUT -std=c17 \
    -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs build/host/fs/ata_dma_fixture.o \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/fat.c src/kernel/fs/path.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    src/kernel/fs/iso9660.c src/kernel/fs/ntfs_fs.c src/kernel/fs/ntfs_ro.c src/kernel/fs/mount.c \
    src/kernel/core/bootlog.c src/kernel/probes/fat_probes.c src/kernel/lib/sha256.c src/kernel/lib/fmt.c \
    tests/host/fs/test_throughput.c -o build/host/fs/test_throughput_dma
build/host/fs/test_throughput_dma
python3 tests/host/fs/kernel_compile.py
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_write_in_place.c \
    -o build/host/fs/test_write_in_place
build/host/fs/test_write_in_place build/host/fs/fat12.img build/host/fs/fat16.img build/host/fs/fat32.img
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs -Isrc/kernel/include \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c src/kernel/fs/ntfs_fs.c src/kernel/fs/ntfs_ro.c src/kernel/fs/mount.c \
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
python3 tests/host/fs/test_crash_classifier.py
python3 tests/host/fs/classify_crashes.py
python3 - <<'PY'
from pathlib import Path
root=Path('build/host/fs')
files=[p for p in root.rglob('*') if p.is_file()]
logical=sum(p.stat().st_size for p in files)
allocated=sum(p.stat().st_blocks*512 for p in files)
assert logical<200_000_000 and allocated<200_000_000,(logical,allocated)
print(f'ARTIFACTS files={len(files)} logical_bytes={logical} allocated_bytes={allocated} limit=200000000')
print('PASS host filesystem suite: 16 legacy groups plus FAT mount-budget cases, 3 fsck checks, 3 independent mtools comparisons')
PY
