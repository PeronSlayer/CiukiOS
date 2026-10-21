#!/usr/bin/env bash
# Optional measured jitter run; never called by host_kernel_tests.sh.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec python3 -B "$root/tests/host/entropy/run.py" --case qualification
