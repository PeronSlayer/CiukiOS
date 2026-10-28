#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host"
mkdir -p "$out"
# sec-k04: net_dev.c's Setup query asks the production credential predicates
# (security.c via the subject fixture); H05 devices_test.c covers that query.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$root/src/kernel/include" "$root/tests/host/net/net_test.c" \
    "$root/src/kernel/lib/fmt.c" \
    "$root/tests/host/security/subject_fixture.c" -o "$out/net_test"
"$out/net_test"
