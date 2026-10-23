#!/usr/bin/env bash
# Host only, called serially by host_fs_tests.sh. No sanitizer policy change.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/host/fs/libsetup
mkdir -p "$out"
export PYTHONDONTWRITEBYTECODE=1
python3 sdk/libdisk/gen_boot.py --out "$out/libdisk_boot.c"
nasm -f bin src/boot/ciukldr.asm -o "$out/ciukldr.bin"
python3 - "$out" <<'PY'
import importlib.util
from pathlib import Path
import sys
spec=importlib.util.spec_from_file_location('image','scripts/build_image.py')
bi=importlib.util.module_from_spec(spec)
spec.loader.exec_module(bi)
out=Path(sys.argv[1])
(out/'loader.bin').write_bytes(bi.prepare_loader((out/'ciukldr.bin').read_bytes()))
(out/'ciukldr.bin').unlink()
PY
python3 tests/host/libsetup_fixture.py "$out"
flags=(-DFS_HOST -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Werror -g -O1
       -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections -pthread
       -Isrc/kernel/include -Isrc/kernel/fs -Isdk/libdisk/include -Isdk/libsetup/include -Itests/host)
clang "${flags[@]}" -include tests/host/libsetup_alloc.h -c sdk/libsetup/txn.c -o "$out/txn.o"
for unit in manifest; do
    clang "${flags[@]}" -c "sdk/libsetup/$unit.c" -o "$out/$unit.o"
done
clang "${flags[@]}" -Dopen=st_native_open -Dread=st_native_read -Dwrite=st_native_write \
    -Dlseek=st_native_seek -Dfsync=st_native_sync -Dclose=st_native_close -Dmkdir=st_native_mkdir \
    -Dtime=st_native_time -c sdk/libsetup/native.c -o "$out/native.o"
clang "${flags[@]}" -Dopen=st_native_open -Dclose=st_native_close -c sdk/libsetup/native_media.c -o "$out/native_media.o"
clang "${flags[@]}" -Dopen=st_native_open -Dwrite=st_native_write -Dfsync=st_native_sync -Dclose=st_native_close \
    -Dmkdir=st_native_mkdir -Drename=st_native_rename -Dunlink=st_native_unlink -c apps/desktop/settings_io.c -o "$out/settings_io.o"
ar rcs "$out/libsetup.a" "$out/txn.o" "$out/manifest.o" "$out/native.o" "$out/native_media.o" "$out/settings_io.o"
clang "${flags[@]}" \
    src/kernel/fs/fs_port.c src/kernel/fs/blkreq.c src/kernel/fs/cache.c src/kernel/fs/partition.c \
    src/kernel/fs/path.c src/kernel/fs/fat.c src/kernel/fs/vfs.c src/kernel/fs/fs_ro.c src/kernel/fs/iso9660.c \
    src/kernel/fs/ntfs_fs.c src/kernel/fs/ntfs_ro.c src/kernel/fs/mount.c src/kernel/fs/pagefile.c src/kernel/fs/disklease.c \
    src/kernel/core/bootlog.c src/kernel/drivers/blkpart.c src/kernel/lib/sha256.c src/kernel/lib/fmt.c \
    sdk/libdisk/disk.c sdk/libdisk/format.c sdk/libdisk/sha256.c "$out/libdisk_boot.c" \
    apps/setup/inf.c apps/setup/catalog.c apps/desktop/settings.c apps/desktop/theme.c apps/control/configuration.c src/kernel/net/config.c \
    sdk/libreg/registry.c \
    tests/host/fs/fake.c tests/host/libdisk_files.c tests/host/libsetup_files.c tests/host/libsetup_test.c \
    -L"$out" -lsetup -Wl,--wrap=storage_writer_start,--gc-sections -o "$out/test"
"$out/test" "$out"

# This runner owns the fixture directory. Keep no synthetic images,
# archives or executables after success (the parent has an artifact budget).
rm -rf "$out"
