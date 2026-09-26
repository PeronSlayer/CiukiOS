#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

WOLF4GW_REPO="${CIUKIOS_WOLF4GW_REPO:-https://github.com/TobiasKarnat/Wolf4GW.git}"
WOLF4GW_COMMIT="${CIUKIOS_WOLF4GW_COMMIT:-a49ead44cb4a6476255e355c4c5e3e48bb7f1d55}"
WATCOM_ROOT="${WATCOM:-/opt/watcom}"
SRC_ROOT="${CIUKIOS_WOLF4GW_BUILD_ROOT:-$CIUKIOS_ROOT/build/external/audio-compat/Wolf4GW-$WOLF4GW_COMMIT}"
OUTPUT_DIR="${CIUKIOS_WOLF4GW_OUTPUT_DIR:-$CIUKIOS_ROOT/build/external/audio-compat/output}"
OUTPUT_EXE="$OUTPUT_DIR/WOLF4GW.EXE"
OUTPUT_NOTICE="$OUTPUT_DIR/WOLF4GW.TXT"

for command_name in git perl sha256sum; do
	command -v "$command_name" >/dev/null 2>&1 \
		|| { echo "[build-wolf4gw] ERROR missing command: $command_name" >&2; exit 1; }
done
for tool_path in "$WATCOM_ROOT/binl64/wpp386" "$WATCOM_ROOT/binl64/wlink"; do
	[[ -x "$tool_path" ]] \
		|| { echo "[build-wolf4gw] ERROR missing OpenWatcom tool: $tool_path" >&2; exit 1; }
done

if [[ ! -d "$SRC_ROOT/.git" ]]; then
	mkdir -p "$(dirname "$SRC_ROOT")"
	git clone --no-checkout "$WOLF4GW_REPO" "$SRC_ROOT"
	git -C "$SRC_ROOT" checkout --detach "$WOLF4GW_COMMIT"
fi

actual_commit="$(git -C "$SRC_ROOT" rev-parse HEAD)"
if [[ "$actual_commit" != "$WOLF4GW_COMMIT" ]]; then
	echo "[build-wolf4gw] ERROR checkout is $actual_commit, expected $WOLF4GW_COMMIT" >&2
	exit 1
fi

# The historical tree contains DOS EOF bytes and case-insensitive include
# names.  Normalize only the private build checkout so OpenWatcom can compile
# it reproducibly on Linux without changing the imported game data.
perl -pi -e 's/\x1a\z//' "$SRC_ROOT/src/foreign.h" "$SRC_ROOT/src/f_spear.h"
# Keyboard is written by IRQ1.  Without volatile, the optimized empty
# key-release loop in Confirm() can cache a pressed key forever while IRQ0
# continues playing music.  Keep the declaration and definition consistent;
# clear the array with volatile stores instead of discarding the qualifier.
perl -0pi -e '
  s/extern\s+boolean\s+Keyboard\[\],\s*JoysPresent\[\];/extern volatile boolean Keyboard[];\r\nextern boolean JoysPresent[];/;
' "$SRC_ROOT/src/id_in.h"
perl -0pi -e '
  s/(?<!volatile )boolean\s+Keyboard\[NumCodes\];/volatile boolean Keyboard[NumCodes];/;
  s/memset \(Keyboard,0,sizeof\(Keyboard\)\);/for (int key = 0; key < NumCodes; ++key) Keyboard[key] = false;/;
' "$SRC_ROOT/src/id_in.cpp"
grep -Fq 'extern volatile boolean Keyboard[];' "$SRC_ROOT/src/id_in.h"
grep -Fq 'volatile boolean Keyboard[NumCodes];' "$SRC_ROOT/src/id_in.cpp"
grep -Fq 'for (int key = 0; key < NumCodes; ++key) Keyboard[key] = false;' "$SRC_ROOT/src/id_in.cpp"
# Accept Enter as an affirmative response in addition to the original Y.
perl -0pi -e '
  s#\Q        } while(!Keyboard[sc_Y] && !Keyboard[sc_N] && !Keyboard[sc_Escape]);\E#        } while(!Keyboard[sc_Y] && !Keyboard[sc_Enter] &&\r\n                !Keyboard[sc_N] && !Keyboard[sc_Escape]);#;
  s#\Q        if (Keyboard[sc_Y])\E#        if (Keyboard[sc_Y] || Keyboard[sc_Enter])#;
  s#\Q        while(Keyboard[sc_Y] || Keyboard[sc_N] || Keyboard[sc_Escape]);\E#        while(Keyboard[sc_Y] || Keyboard[sc_Enter] ||\r\n              Keyboard[sc_N] || Keyboard[sc_Escape]);#;
' "$SRC_ROOT/src/wl_menu.cpp"
for expected_exit_line in \
	'while(!Keyboard[sc_Y] && !Keyboard[sc_Enter] &&' \
	'if (Keyboard[sc_Y] || Keyboard[sc_Enter])' \
	'while(Keyboard[sc_Y] || Keyboard[sc_Enter] ||'; do
	grep -Fq -- "$expected_exit_line" "$SRC_ROOT/src/wl_menu.cpp" \
		|| { echo "[build-wolf4gw] ERROR failed to install CiukiOS exit hardening" >&2; exit 1; }
done
for header_name in audiosod audiowl6 foreign f_spear gfxv_apo gfxv_sod gfxv_wl6 id_ca id_in id_sd id_us id_vh id_vl wl_def wl_menu; do
	ln -sfn "$header_name.h" "$SRC_ROOT/src/${header_name^^}.H"
done
ln -sfn "$WATCOM_ROOT/h/bios.h" "$SRC_ROOT/src/BIOS.H"
ln -sfn "$WATCOM_ROOT/h/dos.h" "$SRC_ROOT/src/DOS.H"
ln -sfn "$WATCOM_ROOT/h/time.h" "$SRC_ROOT/src/TIME.H"

mkdir -p "$OUTPUT_DIR"
echo "[build-wolf4gw] compiling pinned Wolf4GW source at $WOLF4GW_COMMIT"
(
	cd "$SRC_ROOT/src"
	export WATCOM="$WATCOM_ROOT"
	export PATH="$WATCOM_ROOT/binl64:$PATH"
	"$WATCOM_ROOT/binl64/wpp386" \
		-i=. -i="$WATCOM_ROOT/h" -bt=dos -5 -fp5 -fpi87 -ohx -j -zp1 -zq -w0 ub.cpp
	"$WATCOM_ROOT/binl64/wlink" \
		name "$OUTPUT_EXE" system dos4g option eliminate file ub.o
)

[[ -s "$OUTPUT_EXE" ]] \
	|| { echo "[build-wolf4gw] ERROR output missing: $OUTPUT_EXE" >&2; exit 1; }
cp "$SRC_ROOT/README.rst" "$OUTPUT_NOTICE"
echo "[build-wolf4gw] ready: $OUTPUT_EXE"
sha256sum "$OUTPUT_EXE"
