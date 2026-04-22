#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
ciuk_macos_prepare_tools
ciuk_macos_check_required

cd "$CIUKIOS_ROOT"
exec bash scripts/build_floppy.sh "$@"
