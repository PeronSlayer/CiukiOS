#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/vm"
mkdir -p "$out"
flags=(-std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src/kernel/include")
clang "${flags[@]}" "$root/tests/host/vm/bridge_test.c" "$root/src/kernel/net/bridge.c" -o "$out/bridge_test"
"$out/bridge_test"
clang "${flags[@]}" "$root/tests/host/vm/ne2000_test.c" "$root/src/kernel/vm/dosvm_ne2000.c" -o "$out/ne2000_test"
"$out/ne2000_test"
clang "${flags[@]}" "$root/tests/host/vm/ipx_test.c" "$root/src/kernel/vm/dosvm_ipx.c" "$root/src/kernel/net/bridge.c" -o "$out/ipx_test"
"$out/ipx_test"
