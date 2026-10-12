#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" "$root/tests/host/net/net_test.c" \
    "$root/src/kernel/lib/fmt.c" -o "$out/net_test"
"$out/net_test"
