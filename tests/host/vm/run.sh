#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/vm"
mkdir -p "$out"
flags=(-std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src/kernel/include")
gc=(-ffunction-sections -fdata-sections -Wl,--gc-sections)
clang "${flags[@]}" "$root/tests/host/vm/bridge_test.c" "$root/src/kernel/net/bridge.c" -o "$out/bridge_test"
clang "${flags[@]}" "$root/tests/host/vm/ne2000_test.c" "$root/src/kernel/vm/dosvm_ne2000.c" -o "$out/ne2000_test"
clang "${flags[@]}" "$root/tests/host/vm/ipx_test.c" "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/net/bridge.c" -o "$out/ipx_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/tap_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/tap_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/port_test.c" "$root/src/kernel/net/bridge.c" "$root/src/kernel/lib/fmt.c" -o "$out/port_test"
clang "${flags[@]}" "$root/tests/host/vm/settings_test.c" -o "$out/settings_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/network_test.c" "$root/src/kernel/net/bridge.c" \
    "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/vm/dosvm_ne2000.c" "$root/src/kernel/lib/fmt.c" -o "$out/network_test"
clang "${flags[@]}" "$root/tests/host/vm/irq_test.c" "$root/src/kernel/vm/dosvm_ne2000.c" \
    "$root/src/kernel/vm/v86.c" "$root/src/kernel/vm/dosvm_pic.c" -o "$out/irq_test"
for test in bridge ne2000 ipx tap port settings network irq; do
    (cd "$root" && "$out/${test}_test")
done
