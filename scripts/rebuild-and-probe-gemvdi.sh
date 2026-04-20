#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
make build/stage2.elf 2>&1 | tail -5
export CIUKIOS_QEMU_SKIP_RUN=1
./run_ciukios.sh >/tmp/ciukios-build.log 2>&1 || { echo BUILD_FAIL; tail -30 /tmp/ciukios-build.log; exit 1; }
echo BUILD_OK
bash "$(dirname "$0")/run-gemvdi-probe.sh"
