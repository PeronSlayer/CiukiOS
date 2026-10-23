#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/joystick"
mkdir -p "$out/settings"
export TMPDIR="$out"
clang -D_POSIX_C_SOURCE=200809L -std=c17 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/joystick/storage_test.c" \
    "$root/src/kernel/drivers/joystick_storage.c" "$root/src/kernel/lib/fmt.c" -o "$out/storage_test"
status=0
"$out/storage_test" || status=$?
clang -D_POSIX_C_SOURCE=200809L "-DDOSBOX_SETTINGS_DIR=\"$out/settings\"" -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -include "$root/tests/host/joystick/shim.h" -I "$root/src/kernel/include" \
    "$root/tests/host/joystick/persistence_test.c" "$root/src/kernel/drivers/joystick_settings.c" "$root/src/kernel/lib/fmt.c" "$root/src/kernel/drivers/gameport.c" \
    "$root/src/kernel/drivers/joystick_virtual.c" "$root/src/kernel/usb/hid_input.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/dosvm_joystick.c" "$root/src/kernel/vm/v86.c" \
    "$root/src/kernel/vm/dosvm_pic.c" -o "$out/persistence_test"
"$out/persistence_test" || status=$?
clang -D_POSIX_C_SOURCE=200809L "-DDOSBOX_SETTINGS_DIR=\"$out/settings\"" -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -include "$root/tests/host/joystick/shim.h" -I "$root/src/kernel/include" \
    "$root/tests/host/joystick/joystick_test.c" "$root/src/kernel/drivers/gameport.c" \
    "$root/src/kernel/drivers/joystick_virtual.c" "$root/src/kernel/usb/hid_input.c" "$root/src/kernel/lib/sha256.c" \
    "$root/src/kernel/vm/dosvm_joystick.c" "$root/src/kernel/vm/v86.c" \
    "$root/src/kernel/vm/dosvm_pic.c" -o "$out/joystick_test"
"$out/joystick_test" || status=$?
exit "$status"
