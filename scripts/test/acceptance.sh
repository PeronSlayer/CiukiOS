#!/usr/bin/env bash
# fx-accept: the owner's T23 trial list on QEMU qemu-t23, before every hardware
# image (owner decision 2026-10-27, docs/directives/fx-accept.md). A failing
# item blocks the image.
#
#   scripts/test/acceptance.sh                      the whole suite acc-t23
#   scripts/test/acceptance.sh --case acc-quake3    one case (repeatable)
#   scripts/test/acceptance.sh --image PATH         another canonical image
#   scripts/test/acceptance.sh --list               all case IDs (host-only)
#
# run.py runs the cases one at a time, each QEMU in its own systemd user scope
# (MemoryMax=1500M, MemorySwapMax=0) on a copy-on-write overlay of the
# canonical image; it refuses while another qemu-system or an image build
# runs, or when MemAvailable is below 2 GiB. Passing runs keep result.json,
# serial.log and the evaluated screendumps; failing runs also keep their
# overlay, generated CD images and failure screendumps. The one-page summary
# is build/test-runs/acc-t23/summary.txt (summary.json beside it).
# Exit status: 0 ACCEPTED, 1 BLOCKED or INCOMPLETE, 2 refused.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
if [ "${1:-}" = --list ]; then
    python3 -B - <<'PY'
import json
from pathlib import Path
for case in json.loads(Path('tests/suites/acc-t23.json').read_text())['cases']:
    print(case['id'] + '\t' + case['title'])
PY
    exit $?
fi
image=build/f0/ciukios.img
for arg in "$@"; do
    case $arg in --image=*) image=${arg#--image=} ;; esac
done
previous=
for arg in "$@"; do
    [ "$previous" = --image ] && image=$arg
    previous=$arg
done
if [ ! -f "$image" ]; then
    echo "acceptance: canonical image missing: $image (make build-full first)" >&2
    exit 2
fi
free_kib=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
echo "acceptance: image $(sha256sum "$image" | cut -c1-16)... MemAvailable $((free_kib / 1024)) MiB, started $(date '+%Y-%m-%d %H:%M:%S')"
mkdir -p build/test-runs/acc-t23
marker=build/test-runs/acc-t23/.started
touch "$marker"
python3 scripts/test/run.py acc-t23 "$@"
status=$?
# A refusal between cases (another QEMU, a build, low memory) leaves the
# checkpoint summary.json of this run: write its page too.
summary=build/test-runs/acc-t23/summary.json
if [ "$status" -eq 2 ] && [ "$summary" -nt "$marker" ]; then
    python3 scripts/test/acceptance.py summarize "$summary"
fi
rm -f "$marker"
echo "acceptance: finished $(date '+%Y-%m-%d %H:%M:%S'), status $status; summary build/test-runs/acc-t23/summary.txt"
exit $status
