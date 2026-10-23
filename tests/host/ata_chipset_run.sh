#!/usr/bin/env bash
# Sanitized production ATA harness; models are independent invocations.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
mkdir -p build/host
export TMPDIR="$root/build/host" PYTHONDONTWRITEBYTECODE=1
nice -n 19 clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I src/kernel/include \
    tests/host/ata_test.c src/kernel/fs/partition.c src/kernel/fs/fs_port.c \
    src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/fat.c src/kernel/fs/path.c \
    src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    src/kernel/fs/ntfs_fs.c src/kernel/fs/ntfs_ro.c src/kernel/fs/mount.c \
    src/kernel/core/bootlog.c src/kernel/probes/fat_probes.c src/kernel/lib/sha256.c \
    src/kernel/lib/fmt.c tests/host/entropy/unavailable_stub.c -o build/host/ata_test
for family in piix via sis ali nv resources transaction addin-promise addin-highpoint addin-resources addin-raid addin-faults addin-transactions; do
    nice -n 19 build/host/ata_test "$family"
done
nice -n 19 python3 scripts/test/iso_fixtures.py --out build/host/optical --joliet > build/host/optical-manifest.json
nice -n 19 build/host/ata_test
