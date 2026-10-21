#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/vm"
mkdir -p "$out"
pool="$(python3 "$root/tests/host/vm/pool_build.py" "$root" "$out")"
flags=(-std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src/kernel/include")
gc=(-ffunction-sections -fdata-sections -Wl,--gc-sections)
flags+=("${gc[@]}" -pthread)
pool_flags=("$pool" -Wl,-u,pool_fixture_start)
clang "${flags[@]}" "$root/tests/host/vm/bridge_test.c" "$root/src/kernel/net/bridge.c" "${pool_flags[@]}" -o "$out/bridge_test"
clang "${flags[@]}" "$root/tests/host/vm/ne2000_test.c" "$root/src/kernel/vm/dosvm_ne2000.c" -o "$out/ne2000_test"
clang "${flags[@]}" "$root/tests/host/vm/ipx_test.c" "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/net/bridge.c" "${pool_flags[@]}" -o "$out/ipx_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/tap_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/tap_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/port_test.c" "$root/src/kernel/net/bridge.c" "$root/src/kernel/lib/fmt.c" "${pool_flags[@]}" -o "$out/port_test"
clang "${flags[@]}" "$root/tests/host/vm/primary_test.c" "$root/src/kernel/net/bridge.c" "$root/src/kernel/lib/fmt.c" "${pool_flags[@]}" -o "$out/primary_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/identity_test.c" "$root/src/kernel/net/bridge.c" "$root/src/kernel/lib/fmt.c" "${pool_flags[@]}" -o "$out/identity_test"
clang "${flags[@]}" -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread -I "$root/src/kernel/fs" \
    "$root/tests/host/vm/identity_vfs_test.c" "$root/src/kernel/net/bridge_identity.c" \
    "$root/src/kernel/fs/fs_port.c" "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/fs/cache.c" \
    "$root/src/kernel/fs/path.c" "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/vfs.c" \
    "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" -o "$out/identity_vfs_test"
clang "${flags[@]}" "$root/tests/host/vm/settings_test.c" -o "$out/settings_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/network_test.c" "$root/src/kernel/net/bridge.c" \
    "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/vm/dosvm_ne2000.c" "$root/src/kernel/lib/fmt.c" "${pool_flags[@]}" -o "$out/network_test"
clang "${flags[@]}" "$root/tests/host/vm/irq_test.c" "$root/src/kernel/vm/dosvm_ne2000.c" \
    "$root/src/kernel/vm/v86.c" "$root/src/kernel/vm/dosvm_pic.c" -o "$out/irq_test"
clang "${flags[@]}" "${gc[@]}" "$root/tests/host/vm/receive_mode_test.c" "$root/src/kernel/lib/fmt.c" -o "$out/receive_mode_test"
for test in bridge ne2000 ipx tap port primary identity identity_vfs settings network irq receive_mode; do
    (cd "$root" && "$out/${test}_test")
done
bash "$root/tests/host/vm/run_lifecycle.sh"
