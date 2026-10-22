#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/hid"
mkdir -p "$out"
export TMPDIR="$out"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I "$root/src/kernel/include" \
    "$root/tests/host/usb/hid_parser_test.c" "$root/src/kernel/usb/hid_report.c" -o "$out/hid_parser_test"
"$out/hid_parser_test"
