#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/usb"
mkdir -p "$out"
export TMPDIR="$out"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/usb_test.c" \
    "$root/src/kernel/usb/usb_core.c" "$root/src/kernel/usb/uhci.c" \
    "$root/src/kernel/usb/ohci.c" "$root/src/kernel/usb/usb_hub.c" \
    "$root/src/kernel/usb/usb_hid.c" "$root/src/kernel/usb/usb_storage.c" "$root/src/kernel/lib/fmt.c" -o "$out/usb_test"
ASAN_OPTIONS=detect_leaks=0 "$out/usb_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/service_test.c" \
    "$root/src/kernel/usb/uhci.c" "$root/src/kernel/usb/ohci.c" \
    "$root/src/kernel/usb/usb_hub.c" "$root/src/kernel/usb/usb_hid.c" \
    "$root/src/kernel/usb/usb_storage.c" "$root/src/kernel/lib/fmt.c" -o "$out/service_test"
for mode in ready csc-cleared empty create-failed safe forced firmware none; do
    ASAN_OPTIONS=detect_leaks=0 "$out/service_test" "$mode"
done
