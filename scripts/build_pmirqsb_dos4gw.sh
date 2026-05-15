#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

WATCOM_ROOT="${WATCOM:-/opt/watcom}"
WCL386="${WCL386:-$WATCOM_ROOT/binl64/wcl386}"
PMIRQSB_SRC="${PMIRQSB_SRC:-src/probes/pmirqsb/pmirqsb.c}"
PMIRQSB_BIN="${PMIRQSB_BIN:-build/full/obj/pmirqsb.le}"
PMIRQSB_OBJ="${PMIRQSB_OBJ:-build/full/obj/pmirqsb.o}"

if [[ ! -x "$WCL386" ]]; then
	echo "[pmirqsb-build] SKIP OpenWatcom wcl386 not found at $WCL386" >&2
	exit 2
fi

if [[ ! -f "$PMIRQSB_SRC" ]]; then
	echo "[pmirqsb-build] ERROR source not found: $PMIRQSB_SRC" >&2
	exit 1
fi

mkdir -p "$(dirname "$PMIRQSB_BIN")"
echo "[pmirqsb-build] building $PMIRQSB_BIN"
WATCOM="$WATCOM_ROOT" \
INCLUDE="$WATCOM_ROOT/h" \
PATH="$WATCOM_ROOT/binl64:$WATCOM_ROOT/binl:$PATH" \
	"$WCL386" -zq -bt=dos -l=dos4g -fo="$PMIRQSB_OBJ" -fe="$PMIRQSB_BIN" "$PMIRQSB_SRC"
