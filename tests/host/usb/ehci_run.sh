#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/usb"
mkdir -p "$out"
export TMPDIR="$out"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/ehci_test.c" \
    "$root/src/kernel/usb/usb_core.c" "$root/src/kernel/usb/ehci.c" \
    "$root/src/kernel/usb/uhci.c" "$root/src/kernel/usb/ohci.c" \
    "$root/src/kernel/usb/usb_interrupt.c" "$root/src/kernel/usb/usb_hub.c" \
    "$root/src/kernel/usb/usb_hid.c" "$root/src/kernel/usb/hid_report.c" \
    "$root/src/kernel/usb/hid_input.c" "$root/tests/host/usb/hid_support.c" \
    "$root/src/kernel/usb/usb_storage.c" \
    "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/lib/fmt.c" -o "$out/ehci_test"
status=0
"$out/ehci_test" || status=$?
python3 "$root/tests/host/fs/fixtures.py"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/ehci_storage_test.c" \
    "$root/src/kernel/usb/usb_core.c" "$root/src/kernel/usb/ehci.c" \
    "$root/src/kernel/usb/usb_storage.c" "$root/src/kernel/usb/uhci.c" \
    "$root/src/kernel/usb/usb_interrupt.c" "$root/src/kernel/lib/fmt.c" \
    "$root/src/kernel/core/bootlog.c" "$root/src/kernel/fs/fs_port.c" \
    "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/fs/cache.c" \
    "$root/src/kernel/fs/partition.c" "$root/src/kernel/fs/path.c" \
    "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/vfs.c" \
    "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" \
    "$root/src/kernel/drivers/blkpart.c" "$root/src/kernel/fs/mount.c" \
    -o "$out/ehci_storage_test"
"$out/ehci_storage_test" "$root/build/host/fs/fat16.img" || status=$?
exit "$status"
