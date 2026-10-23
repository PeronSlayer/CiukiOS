#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$root/build/host/usb"
mkdir -p "$out"
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1 ASAN_OPTIONS=detect_leaks=0
python3 "$root/tests/host/fs/fixtures.py"
build_test() {
clang -std=c17 -O1 -g -Wall -Wextra -Werror -DCIUKI_USB_HOST -DFS_HOST -D_POSIX_C_SOURCE=200809L -pthread \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$root/src/kernel/include" "$root/tests/host/usb/$1.c" \
    "$root/src/kernel/usb/usb_core.c" "$root/src/kernel/usb/ehci.c" "$root/src/kernel/usb/usb_storage.c" \
    "$root/src/kernel/usb/uhci.c" "$root/src/kernel/usb/usb_interrupt.c" "$root/src/kernel/lib/fmt.c" \
    "$root/src/kernel/core/bootlog.c" "$root/src/kernel/fs/fs_port.c" "$root/src/kernel/fs/blkreq.c" "$root/src/kernel/fs/cache.c" \
    "$root/src/kernel/fs/partition.c" "$root/src/kernel/fs/path.c" \
    "$root/src/kernel/fs/fat.c" "$root/src/kernel/fs/vfs.c" \
    "$root/src/kernel/fs/fs_ro.c" "$root/src/kernel/fs/iso9660.c" \
    "$root/src/kernel/drivers/blkpart.c" "$root/src/kernel/fs/mount.c" \
    -I "$out" -o "$out/$1"
}
build_test storage_test
"$out/storage_test" "$root/build/host/fs/fat12.img" "$root/build/host/fs/fat16.img" "$root/build/host/fs/fat32.img"
build_test f1_74_test
"$out/f1_74_test" "$root/build/host/fs/fat16.img"
build_test f1_81_test
"$out/f1_81_test" "$root/build/host/fs/fat16.img"
# f1-96: negotiated 64 KiB READ(10)/WRITE(10), TD ring, fallback, 5 MB copy.
build_test f1_96_test
"$out/f1_96_test" "$root/build/host/fs/fat16.img"

# Replay the real run queue policy while the USB bind worker sleeps.
python3 - "$root" "$out" <<'PYTHON'
from pathlib import Path
import re, sys
root, out = map(Path, sys.argv[1:])
s = (root / "src/kernel/core/task.c").read_text()
body = s[s.index("static struct task *rq_head"):s.index("static struct task *all_tasks")].replace("static ","static __attribute__((unused)) ")
for name in ("rq_remove", "priority_base", "priority_init", "rq_link", "rq_push", "tick_reached", "device_throttled", "device_urgent", "rq_take", "rq_pop"):
    match = re.search(r"^static [^\n]+\b"+name+r"\([^\n]*\)\n\{",s,re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (s[end]=="{")-(s[end]=="}"); end += 1
    body += s[match.start():end]+"\n"
(out / "usb_bind_queues.inc").write_text(body)
PYTHON
build_test bind_freeze_test
"$out/bind_freeze_test" "$root/build/host/fs/fat12.img" "$root/build/host/fs/fat16.img" "$root/build/host/fs/fat32.img"
# f1-96b: drained TD ring, early-Intel element erratum, same-chain retry, 128/8/1 ladder.
build_test f1_96b_test
"$out/f1_96b_test" "$root/build/host/fs/fat16.img"
# fx-21: a stick enumerated through the full UHCI schedule (PORTSC reset and
# PE, address 0, SET_ADDRESS, descriptors, BOT bind, E: mount, fx-17 loop).
build_test fx21_test
"$out/fx21_test" "$root/build/host/fs/fat16.img"
