#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

WATCOM_ROOT="${WATCOM:-/opt/watcom}"
WCL386="${WCL386:-$WATCOM_ROOT/binl64/wcl386}"
DOOMSFX_SRC="${DOOMSFX_SRC:-src/probes/doomsfx/doomsfx.c}"
DOOMSFX_BIN="${DOOMSFX_BIN:-build/full/obj/doomsfx.le}"
DOOMSFX_OBJ="${DOOMSFX_OBJ:-build/full/obj/doomsfx.o}"

if [[ ! -x "$WCL386" ]]; then
	echo "[doomsfx-build] SKIP OpenWatcom wcl386 not found at $WCL386" >&2
	exit 2
fi

if [[ ! -f "$DOOMSFX_SRC" ]]; then
	echo "[doomsfx-build] ERROR source not found: $DOOMSFX_SRC" >&2
	exit 1
fi

mkdir -p "$(dirname "$DOOMSFX_BIN")"
echo "[doomsfx-build] building $DOOMSFX_BIN"
WATCOM="$WATCOM_ROOT" \
INCLUDE="$WATCOM_ROOT/h" \
PATH="$WATCOM_ROOT/binl64:$WATCOM_ROOT/binl:$PATH" \
	"$WCL386" -zq -bt=dos -l=dos4g -fo="$DOOMSFX_OBJ" -fe="$DOOMSFX_BIN" "$DOOMSFX_SRC"
