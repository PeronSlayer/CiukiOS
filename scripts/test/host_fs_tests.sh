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
for tool in clang ar python3 mkfs.fat fsck.fat mcopy mmd mdir mtype mdel nm nasm; do
    if ! command -v "$tool" >/dev/null; then printf 'MISSING TOOL: %s\n' "$tool"; missing=1; fi
done
if [[ ${1:-} == --owner-policy ]]; then
    ((missing == 0)) || exit 1
    python3 -B -m unittest discover -s tests/host/security -p test_owner_policy.py
    exit 0
fi
# The RAM logger's production pacing/retention model uses this runner's
# standard filesystem sanitizer policy; no disk/image fixture is needed.
if [[ ${1:-} == --bootlog ]]; then
    command -v clang >/dev/null || exit 1
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -Isrc/kernel/include \
        tests/host/bootlog_test.c -o build/host/fs/bootlog_test
    build/host/fs/bootlog_test
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -Isrc/kernel/include \
        tests/host/bootlog_statistics_test.c src/kernel/lib/fmt.c -o build/host/fs/bootlog_statistics_test
    build/host/fs/bootlog_statistics_test
    exit 0
fi
# Focused entries keep the filesystem runner lock and sanitizer policy.
run_startup_image_tests() {
    ((missing == 0)) || exit 1
    python3 tests/host/fs/fixtures.py
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -Isrc/kernel/include -Isrc/kernel/fs \
        tests/host/proc/startup_image_test.c tests/host/proc/signal_legacy.c \
        tests/host/proc/desktop_legacy.c src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c \
        src/kernel/fs/cache.c src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c \
        src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c tests/host/fs/fake.c \
        src/kernel/probes/f2_probes_files.c -o build/host/fs/startup_image_test
    build/host/fs/startup_image_test build/host/fs/fat32.img build/host/fs/fat16.img
    nm --defined-only build/host/fs/startup_image_test > build/host/fs/startup-image-symbols.txt
    rm build/host/fs/startup_image_test
}
if [[ ${1:-} == --launch-replay ]]; then
    clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
        -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
        -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
        tests/host/fs/test_launch_replay.c -o build/host/fs/test_launch_replay
    build/host/fs/test_launch_replay
    rm build/host/fs/test_launch_replay
    exit 0
fi
if [[ ${1:-} == --startup-image ]]; then
    run_startup_image_tests
    exit 0
fi
if [[ ${1:-} == --boot-convert ]]; then
    ((missing == 0)) || exit 1
    python3 tests/host/test_boot_conversion.py
    exit 0
fi
if [[ ${1:-} == --fat12 ]]; then
    if ((missing)); then echo 'Fixture tests NOT RUN: required tool missing'; exit 1; fi
    python3 tests/host/fs/fat12.py
    exit 0
fi
if [[ ${1:-} == --libsetup ]]; then
    ((missing == 0)) || exit 1
    bash scripts/test/host_libsetup_tests.sh
    exit 0
fi
run_format_tests() {
    ((missing == 0)) || exit 1
    local out=build/host/fs/format type size
    mkdir -p "$out"
    for type in 12 16 32; do
        case $type in 12) size=1440K;; 16) size=16M;; 32) size=64M;; esac
        truncate -s "$size" "$out/fat$type.img"
        mkfs.fat -F "$type" -s 1 "$out/fat$type.img"
    done
    clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
        -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c \
        src/kernel/fs/cache.c src/kernel/fs/partition.c src/kernel/fs/path.c src/kernel/fs/fat.c \
        src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c src/kernel/fs/ntfs_ro.c \
        src/kernel/fs/ntfs_fs.c src/kernel/fs/mount.c src/kernel/fs/disklease.c src/kernel/drivers/blkpart.c \
        src/kernel/lib/fmt.c tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_format.c -o "$out/test_format"
    "$out/test_format" "$out/fat12.img" "$out/fat16.img" "$out/fat32.img"
    for type in 12 16 32; do fsck.fat -n "$out/fat$type.img"; done
    rm "$out/test_format" "$out/fat12.img" "$out/fat16.img" "$out/fat32.img"
}
run_maintenance_mount_tests() {
    ((missing == 0)) || exit 1
    local out=build/host/fs/maintenance-mount
    mkdir -p "$out"
    truncate -s $(((66640+2048)*512)) "$out/fixed.img"
    mkfs.fat -F 16 -s 2 -h 2048 --offset 2048 "$out/fixed.img" 33320
    clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
        -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c \
        src/kernel/fs/cache.c src/kernel/fs/partition.c src/kernel/fs/path.c src/kernel/fs/fat.c \
        src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c src/kernel/fs/ntfs_ro.c \
        src/kernel/fs/ntfs_fs.c src/kernel/fs/mount.c src/kernel/drivers/blkpart.c \
        src/kernel/lib/fmt.c tests/host/fs/fake.c tests/host/fs/test_maintenance_mount.c -o "$out/test_mount"
    nasm -f bin src/boot/mbr.asm -o "$out/mbr.bin"
    nasm -f bin src/boot/ciukldr.asm -o "$out/loader.bin"
    python3 - "$out/loader.bin" <<'PYLOADER'
import pathlib,struct,sys,zlib
p=pathlib.Path(sys.argv[1]); b=bytearray(p.read_bytes())
struct.pack_into('<H',b,6,48); struct.pack_into('<I',b,8,zlib.crc32(b[16:])); p.write_bytes(b)
PYLOADER
    "$out/test_mount" "$out/fixed.img" "$out/mbr.bin" "$out/loader.bin"
    rm "$out/test_mount" "$out/fixed.img" "$out/mbr.bin" "$out/loader.bin"
}
if [[ ${1:-} == --maintenance-mount ]]; then
    run_maintenance_mount_tests
    exit 0
fi
run_convert_tests() {
    ((missing == 0)) || exit 1
    local out=build/host/fs/convert
    mkdir -p "$out"
    truncate -s $(((66640+2048)*512)) "$out/fixed.img"
    mkfs.fat -F 16 -s 2 -h 2048 --offset 2048 "$out/fixed.img" 33320
    clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
        -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c \
        src/kernel/fs/cache.c src/kernel/fs/path.c src/kernel/fs/fat.c \
        tests/host/fs/fake.c tests/host/fs/test_convert.c -o "$out/test_convert"
    "$out/test_convert" "$out/fixed.img"
    # Extract only the small fixture partition for the independent checker.
    dd if="$out/fixed.img" of="$out/volume.img" bs=512 skip=2048 count=66640 status=none
    fsck.fat -n "$out/volume.img"
    mdir -i "$out/volume.img" ::
    rm "$out/test_convert" "$out/fixed.img" "$out/volume.img"
    run_maintenance_mount_tests
}
if [[ ${1:-} == --convert ]]; then
    run_convert_tests
    exit 0
fi
run_defrag_tests() {
    ((missing == 0)) || exit 1
    local out=build/host/fs/defrag type size
    mkdir -p "$out"
    for type in 12 16 32; do
        case $type in 12) size=1440K;; 16) size=16M;; 32) size=64M;; esac
        truncate -s "$size" "$out/fat$type.img"
        mkfs.fat -F "$type" -s 1 "$out/fat$type.img"
    done
    clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
        -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c \
        src/kernel/fs/cache.c src/kernel/fs/path.c src/kernel/fs/fat.c \
        tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_defrag.c -o "$out/test_defrag"
    "$out/test_defrag" "$out/fat12.img" "$out/fat16.img" "$out/fat32.img"
    for type in 12 16 32; do fsck.fat -n "$out/fat$type.img"; done
    rm "$out/test_defrag" "$out/fat12.img" "$out/fat16.img" "$out/fat32.img"
}
run_startup_tests() {
    ((missing == 0)) || exit 1
    local out=build/host/fs/startup type size
    mkdir -p "$out"
    for type in 12 16 32; do
        case $type in 12) size=1440K;; 16) size=16M;; 32) size=64M;; esac
        truncate -s "$size" "$out/fat$type.img"
        mkfs.fat -F "$type" -s 1 "$out/fat$type.img"
    done
    clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
        -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
        -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c \
        src/kernel/fs/cache.c src/kernel/fs/path.c src/kernel/fs/fat.c \
        tests/host/fs/fake.c tests/host/fs/scan.c tests/host/fs/test_startup.c -o "$out/test_startup"
    "$out/test_startup" "$out/fat12.img" "$out/fat16.img" "$out/fat32.img"
    for type in 12 16 32; do fsck.fat -n "$out/fat$type.img"; done
    rm "$out/test_startup" "$out/fat12.img" "$out/fat16.img" "$out/fat32.img"
}
if [[ ${1:-} == --startup ]]; then
    run_startup_tests
    exit 0
fi
if [[ ${1:-} == --defrag ]]; then
    run_defrag_tests
    exit 0
fi
if [[ ${1:-} == --format ]]; then
    run_format_tests
    exit 0
fi
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
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/fs_ro.c \
    tests/host/fs/test_vfs_urgent_admission.c -o build/host/fs/test_vfs_urgent_admission
build/host/fs/test_vfs_urgent_admission
rm build/host/fs/test_vfs_urgent_admission
# f1-87: T23 request mix and 111 ms device commands. The strict amendment-1
# gate also requires bounded logical holds; diagnostics alone cannot pass B.
# f1-93 (unconditional): the 1.0.0 alpha cold paths at 106 ms per command,
# including the DOS VM's logical C: through the production dos_file_volume.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/vm/dos_file_volume.c \
    tests/host/fs/test_t23_holds.c -Wl,--wrap=fs_lock_acquired,--wrap=fs_lock_released -o build/host/fs/test_t23_holds
tag_status=0
build/host/fs/test_t23_holds --missing-tag || tag_status=$?
if [[ $tag_status != 1 ]]; then
    echo 'FAIL: untagged volume hold was accepted' >&2; exit 1
fi
if [[ ${CIUKI_F187_REQUIRE_BOUNDED:-0} == 1 ]]; then
    build/host/fs/test_t23_holds --require-bounded
else
    build/host/fs/test_t23_holds
fi
rm build/host/fs/test_t23_holds
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    tests/host/fs/test_fat_transactions.c -o build/host/fs/test_fat_transactions
build/host/fs/test_fat_transactions
rm build/host/fs/test_fat_transactions
# f1-89: concurrent writers/namespace mutators/readers through the public
# VFS; no internal EAGAIN may surface; remount and fsck must be clean.
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c \
    tests/host/fs/test_fat_concurrency.c -o build/host/fs/test_fat_concurrency
build/host/fs/test_fat_concurrency build/host/fs/concurrency.img
fsck.fat -n build/host/fs/concurrency.img
rm build/host/fs/concurrency.img build/host/fs/test_fat_concurrency
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
# fx-launch: the T23 trial 2ca3b0b6 starts replayed on the production cache
# (C4 sizing from the trial's ledger, PATA timing from verbose trials).
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    tests/host/fs/test_launch_replay.c -o build/host/fs/test_launch_replay
build/host/fs/test_launch_replay
rm build/host/fs/test_launch_replay
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c \
    tests/host/fs/test_cache_reservation.c -o build/host/fs/test_cache_reservation
build/host/fs/test_cache_reservation
rm build/host/fs/test_cache_reservation
# T23 boot: late FAT32 arrival beside display-marker/boot-log publication.
python3 -B tests/host/test_t23_boot_hang.py
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
python3 tests/host/fs/fat12.py
python3 tests/host/fs/fixtures.py
# Native desktop path/fd adapters must meet the same cold-device hold bound;
# f1-93 adds stat, executable image read, readdir and open at 106 ms.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    tests/host/proc/files_holds_test.c tests/host/proc/signal_legacy.c tests/host/proc/desktop_legacy.c \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/path.c \
    src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c tests/host/fs/fake.c \
    src/kernel/probes/f2_probes_files.c -Wl,--wrap=fs_lock_acquired,--wrap=fs_lock_released \
    -o build/host/fs/files_holds_test
build/host/fs/files_holds_test build/host/fs/fat32.img build/host/fs/fat16.img
# f1-93b: the T23 b453bc92 desktop#31 directory compare (2,000 entries, cold
# cache, 106 ms per command, a starved holder that loses 100 ms at every
# preemption point), on the T23 C: geometry (FAT32, 4 KiB clusters, as
# scripts/build_image.py) and on 512-byte clusters. Sorted copy: the
# directory order does not depend on the host file system.
rm -rf build/host/fs/folder-src build/host/fs/folder32.img build/host/fs/folder32s1.img
python3 -c 'import os,sys; r=os.path.join(sys.argv[1],"Desktop"); os.makedirs(r,exist_ok=True); [open(os.path.join(r,"i%04d Desktop item.txt"%i),"wb").close() for i in range(2000)]' build/host/fs/folder-src
truncate -s 288M build/host/fs/folder32.img
mkfs.fat --invariant -F 32 -S 512 -s 8 -f 2 -i 00C1A93B build/host/fs/folder32.img > /dev/null
cp --sparse=always build/host/fs/fat32.img build/host/fs/folder32s1.img
for folder in folder32 folder32s1; do
    MTOOLSRC=build/host/fs/mtoolsrc mmd -i "build/host/fs/$folder.img" ::/Documents ::/Documents/Desktop
    ( export LC_ALL=C; MTOOLSRC=build/host/fs/mtoolsrc mcopy -i "build/host/fs/$folder.img" build/host/fs/folder-src/Desktop/* ::/Documents/Desktop/ )
    MTOOLSRC=build/host/fs/mtoolsrc mmd -i "build/host/fs/$folder.img" "::/Documents/Desktop/Zz Sub"
    build/host/fs/files_holds_test --desktop31 "build/host/fs/$folder.img" build/host/fs/fat16.img
    fsck.fat -n "build/host/fs/$folder.img" > /dev/null
done
rm -rf build/host/fs/folder-src build/host/fs/folder32.img build/host/fs/folder32s1.img
rm build/host/fs/files_holds_test
# f9-02: sdk/libdisk over the production lease code (disklease.c, mount.c,
# fat.c, cache.c) on file-backed disks. Oracles: build_image.py + mkfs.fat
# (libdisk_oracle.py), fsck.fat -n, mtools, the f1-22 corruption fixtures.
rm -rf build/host/fs/libdisk
python3 tests/host/libdisk_oracle.py --out build/host/fs/libdisk
python3 sdk/libdisk/gen_boot.py --out build/host/fs/libdisk/libdisk_boot.c
clang -DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -Isrc/kernel/include -Isrc/kernel/fs -Isdk/libdisk/include -Isdk/libdisk -Itests/host \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    src/kernel/fs/ntfs_fs.c src/kernel/fs/ntfs_ro.c src/kernel/fs/mount.c src/kernel/fs/pagefile.c src/kernel/fs/disklease.c \
    src/kernel/core/bootlog.c src/kernel/drivers/blkpart.c src/kernel/lib/sha256.c src/kernel/lib/fmt.c \
    sdk/libdisk/disk.c sdk/libdisk/format.c sdk/libdisk/sha256.c build/host/fs/libdisk/libdisk_boot.c \
    tests/host/fs/fake.c tests/host/libdisk_files.c tests/host/libdisk_test.c \
    -Wl,--wrap=storage_writer_start -o build/host/fs/libdisk_test
build/host/fs/libdisk_test build/host/fs/libdisk
# The real native boundary: user copies/pins, process inheritance/death,
# raw dispatch and published-volume cold-read reservations after f9-01.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -Isrc/kernel/include -Isrc/kernel/fs \
    tests/host/proc/disk_test.c tests/host/proc/signal_legacy.c tests/host/proc/desktop_legacy.c \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/path.c \
    src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    src/kernel/fs/partition.c src/kernel/fs/mount.c src/kernel/fs/disklease.c \
    src/kernel/drivers/blkpart.c src/kernel/drivers/disk_dev.c tests/host/fs/fake.c \
    -o build/host/fs/native_disk_test
build/host/fs/native_disk_test build/host/fs/fat32.img build/host/fs/libdisk/volume-1g.img
rm build/host/fs/native_disk_test
# Archive extraction is not exercised by directly linking the test objects.
for source in sdk/libdisk/disk.c sdk/libdisk/format.c sdk/libdisk/sha256.c build/host/fs/libdisk/libdisk_boot.c; do
    clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -Isrc/kernel/include -Isdk/libdisk/include -Isdk/libdisk -c "$source" \
        -o "build/host/fs/libdisk/$(basename "$source" .c).o"
done
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isdk/libdisk/include -DLIBDISK_LINK_BACKEND -c tests/host/libdisk_link_test.c \
    -o build/host/fs/libdisk/native.o
ar rcs build/host/fs/libdisk/libdisk.a build/host/fs/libdisk/*.o
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isdk/libdisk/include tests/host/libdisk_link_test.c -Lbuild/host/fs/libdisk -ldisk \
    -o build/host/fs/libdisk/link_test
build/host/fs/libdisk/link_test
rm -rf build/host/fs/libdisk build/host/fs/libdisk_test
# f9-04: installation transaction over the same production FAT/VFS/lease.
bash scripts/test/host_libsetup_tests.sh
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
    src/kernel/usb/usb_core.c src/kernel/usb/ehci.c src/kernel/usb/usb_storage.c src/kernel/usb/uhci.c src/kernel/usb/usb_interrupt.c \
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
    tests/host/fs/test_throughput.c tests/host/entropy/unavailable_stub.c -o build/host/fs/test_throughput_dma
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
    build/host/fs/test_fs txcrash "$image"
    cp --sparse=always "$image" build/host/fs/work.img
    build/host/fs/test_fs write build/host/fs/work.img
    fsck.fat -n build/host/fs/work.img
    python3 tests/host/fs/verify.py build/host/fs/work.img
    rm build/host/fs/work.img
done
# G14 ScanDisk engine: damaged mkfs.fat volumes repaired, then fsck.fat -n.
python3 -B tests/host/fs/test_scandisk.py
python3 tests/host/fs/test_crash_classifier.py
python3 tests/host/fs/classify_crashes.py
run_format_tests
run_defrag_tests
run_startup_tests
run_startup_image_tests
python3 tests/host/test_boot_conversion.py
run_convert_tests
# sec-k02 uses this runner's existing filesystem sanitizer policy. The direct
# H02 command retains its default LeakSanitizer setting.
python3 -B -m unittest discover -s tests/host/security -p test_owner_policy.py
# ---- sec-k03 H04 (one block): subject enforcement on the production VFS. ----
python3 -B -m unittest discover -s tests/host/security -p test_file_enforcement.py
# ---- end sec-k03 ----
python3 - <<'PY'
from pathlib import Path
root=Path('build/host/fs')
files=[p for p in root.rglob('*') if p.is_file()]
logical=sum(p.stat().st_size for p in files)
allocated=sum(p.stat().st_blocks*512 for p in files)
# 2026-10-24: 240 MB; fx-23 and G29b-d added ASan test binaries (about 7 MB each).
assert logical<240_000_000 and allocated<240_000_000,(logical,allocated)
print(f'ARTIFACTS files={len(files)} logical_bytes={logical} allocated_bytes={allocated} limit=240000000')
print('PASS host filesystem suite: 16 legacy groups plus FAT mount-budget cases, 3 fsck checks, 3 independent mtools comparisons')
PY
