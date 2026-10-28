#!/usr/bin/env bash
# fx-accept: the owner's T23 trial list on QEMU qemu-t23, before every hardware
# image (owner decision 2026-10-27, docs/directives/fx-accept.md). A failing
# item blocks the image.
#
#   scripts/test/acceptance.sh                      the whole suite acc-t23
#   scripts/test/acceptance.sh --case acc-quake3    one case (repeatable)
#   scripts/test/acceptance.sh --image PATH         another canonical image
#   scripts/test/acceptance.sh --list               validate the suite and list its cases (no QEMU)
#   scripts/test/acceptance.sh --suite sec-users    another acceptance suite (sec-user-06: the
#                                                   security runtime cases, tests/suites/sec-users.json)
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
image=build/f0/ciukios.img
suite=acc-t23
list=0
args=()
previous=
for arg in "$@"; do
    case $arg in
        --image=*) image=${arg#--image=} ;;
        --suite=*) suite=${arg#--suite=} ;;
        --list) list=1 ;;
    esac
    [ "$previous" = --image ] && image=$arg
    [ "$previous" = --suite ] && suite=$arg
    # --suite and --list are this script's own options; run.py takes the rest.
    case $arg in --suite|--suite=*|--list) ;; *) [ "$previous" = --suite ] || args+=("$arg") ;; esac
    previous=$arg
done
case $suite in acc-t23|sec-users|lan-t23) ;; *) echo "acceptance: unknown acceptance suite: $suite" >&2; exit 2 ;; esac
if [ "$list" -eq 1 ]; then
    # Parses every case exactly as run.py does before a boot; no QEMU, no image.
    python3 -B scripts/test/acceptance.py validate "$suite"
    exit $?
fi
if [ ! -f "$image" ]; then
    echo "acceptance: canonical image missing: $image (make build-full first)" >&2
    exit 2
fi
free_kib=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
echo "acceptance: image $(sha256sum "$image" | cut -c1-16)... MemAvailable $((free_kib / 1024)) MiB, started $(date '+%Y-%m-%d %H:%M:%S')"
mkdir -p "build/test-runs/$suite"
marker=build/test-runs/$suite/.started
touch "$marker"
python3 scripts/test/run.py "$suite" ${args[@]+"${args[@]}"}
status=$?
# A refusal between cases (another QEMU, a build, low memory) leaves the
# checkpoint summary.json of this run: write its page too.
summary=build/test-runs/$suite/summary.json
if [ "$status" -eq 2 ] && [ "$summary" -nt "$marker" ]; then
    python3 scripts/test/acceptance.py summarize "$summary"
fi
rm -f "$marker"
echo "acceptance: finished $(date '+%Y-%m-%d %H:%M:%S'), status $status; summary build/test-runs/$suite/summary.txt"
exit $status
