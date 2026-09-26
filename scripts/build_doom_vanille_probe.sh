#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

DOOM_VANILLE_SRC="${DOOM_VANILLE_SRC:-build/external/doom-vanille}"
WATCOM_ROOT="${WATCOM:-/opt/watcom}"
WCL386="${WCL386:-$WATCOM_ROOT/binl64/wcl386}"

if [[ ! -d "$DOOM_VANILLE_SRC" ]]; then
	echo "[DOOMVANILLE] SOURCE MISSING path=$DOOM_VANILLE_SRC"
	exit 0
fi

if [[ ! -f "$DOOM_VANILLE_SRC/pcdoom.wpj" || ! -f "$DOOM_VANILLE_SRC/pcdoom.tgt" ]]; then
	echo "[DOOMVANILLE] SOURCE MISSING project files absent path=$DOOM_VANILLE_SRC"
	exit 0
fi

if [[ ! -x "$WCL386" ]]; then
	echo "[DOOMVANILLE] TOOLCHAIN MISSING wcl386=$WCL386"
	exit 0
fi

echo "[DOOMVANILLE] BUILD START source=$DOOM_VANILLE_SRC"
if (
	cd "$DOOM_VANILLE_SRC"
	for f in *; do
		lower=$(printf '%s' "$f" | tr '[:upper:]' '[:lower:]')
		if [[ "$f" != "$lower" && ! -e "$lower" ]]; then
			ln -s "$f" "$lower"
		fi
	done
	if grep -q '^boolean mus2mid(FILE \\*musinput, FILE \\*midioutput)' mus2mid.c; then
		perl -0pi -e 's/boolean mus2mid\(FILE \*musinput, FILE \*midioutput\)/int mus2mid(FILE *musinput, FILE *midioutput)/' mus2mid.c
	fi
	WATCOM="$WATCOM_ROOT" \
	INCLUDE="$WATCOM_ROOT/h" \
	PATH="$WATCOM_ROOT/binl64:$WATCOM_ROOT/binl:$PATH" \
		"$WCL386" -zq -i=. -bt=dos -l=dos4g \
			-j -ei -zp=1 -ox -oi -oa -d0 \
			-fe=pcdoom.exe *.c pcfx.obj audio_wf.lib
); then
	echo "[DOOMVANILLE] BUILD PASS"
	exit 0
fi

echo "[DOOMVANILLE] BUILD FAIL"
exit 1
