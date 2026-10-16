#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/usb"
mkdir -p "$out"
export TMPDIR="$out"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/usb_test.c" \
    "$root/src/kernel/usb/usb_core.c" "$root/src/kernel/usb/uhci.c" "$root/src/kernel/usb/usb_interrupt.c" \
    "$root/src/kernel/usb/ohci.c" "$root/src/kernel/usb/usb_hub.c" \
    "$root/src/kernel/usb/usb_hid.c" "$root/src/kernel/usb/usb_storage.c" "$root/src/kernel/lib/fmt.c" -o "$out/usb_test"
ASAN_OPTIONS=detect_leaks=0 "$out/usb_test"
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/service_test.c" \
    "$root/src/kernel/usb/uhci.c" "$root/src/kernel/usb/usb_interrupt.c" "$root/src/kernel/usb/ohci.c" \
    "$root/src/kernel/usb/usb_hub.c" "$root/src/kernel/usb/usb_hid.c" \
    "$root/src/kernel/usb/usb_storage.c" "$root/src/kernel/lib/fmt.c" -o "$out/service_test"
for mode in ready diagnostic csc-cleared empty create-failed input-create-failed hid-worker safe forced firmware none \
    phased-delays done-head-interleave fatal-relatched fatal-persistent fatal-reasserted fatal-gate-stuck \
    fatal-source-stuck fatal-run-stuck fatal-halt-stuck fatal-bme-stuck \
    completion-uhci-irq completion-uhci-lost completion-uhci-poll \
    completion-ohci-irq completion-ohci-lost completion-ohci-poll; do
    ASAN_OPTIONS=detect_leaks=0 "$out/service_test" "$mode"
done
# f1-51: production HID, i8042 merged queue and desktop grants on fake RAM.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/input_grant_test.c" \
    "$root/tests/host/usb/input_queue.c" "$root/src/kernel/usb/usb_hid.c" \
    "$root/tests/host/proc/signal_legacy.c" "$root/src/kernel/lib/fmt.c" \
    "$root/src/kernel/lib/sha256.c" "$root/src/kernel/probes/selector.c" \
    -o "$out/input_grant_test"
ASAN_OPTIONS=detect_leaks=0 "$out/input_grant_test"

# f1-52: both real packet engines, BOT waiting and the real merged input queue.
clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/latency_test.c" \
    "$root/tests/host/usb/input_queue.c" "$root/src/kernel/usb/usb_core.c" \
    "$root/src/kernel/usb/usb_hid.c" "$root/src/kernel/usb/usb_hub.c" \
    "$root/src/kernel/usb/usb_storage.c" "$root/src/kernel/usb/usb_interrupt.c" \
    "$root/src/kernel/usb/uhci.c" "$root/src/kernel/usb/ohci.c" "$root/src/kernel/lib/fmt.c" \
    -o "$out/latency_test"
ASAN_OPTIONS=detect_leaks=0 "$out/latency_test"

# f0-21: exact native task creation and worker loops, independent controllers.
python3 - "$root" "$out" <<'PYTHON'
from pathlib import Path
import re, sys
root, out = map(Path, sys.argv[1:])
s = (root / "src/kernel/usb/usb_core.c").read_text()
body = ""
for name in ("native_now", "service", "input_service", "start_services"):
    m = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\)\s*\{", s, re.M)
    assert m, name
    depth, end = 1, m.end()
    while depth:
        depth += (s[end] == "{") - (s[end] == "}"); end += 1
    body += s[m.start():end] + "\n"
(out / "usb_worker_replay.inc").write_text(body)
PYTHON
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -fsanitize=address,undefined -I "$root/src/kernel/include" -I "$out" \
    "$root/tests/host/usb/workers_test.c" -o "$out/workers_test"
ASAN_OPTIONS=detect_leaks=0 "$out/workers_test"
