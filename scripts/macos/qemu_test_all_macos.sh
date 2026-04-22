#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
ciuk_macos_prepare_tools
ciuk_macos_check_required

cd "$CIUKIOS_ROOT"

bash scripts/macos/build_full_macos.sh
bash scripts/macos/build_floppy_macos.sh
bash scripts/macos/qemu_run_full_macos.sh --test
bash scripts/macos/qemu_run_floppy_macos.sh --test
