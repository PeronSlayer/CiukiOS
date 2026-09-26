#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

HX_REPOSITORY="${CIUKIOS_HX_REPOSITORY:-https://github.com/Baron-von-Riedesel/HX.git}"
# HX v2.24pre1 contains HDPMI 3.24.  It fixes the v3.22-v3.23 regression in
# DPMI 0300h/0301h/0302h real-mode calls when the caller supplies SP=0000
# (a valid 64 KiB real-mode stack), which Windows 3.1 drivers can exercise.
# VSBHDA 2.0 requires the official HDPMI
# port-trapping ABI at version 3.21 or newer; crazii/HX exposes a different,
# SBEMU-specific ABI even though both identify themselves as "HDPMI".
HX_COMMIT="${CIUKIOS_HX_COMMIT:-f2276db9accfc57facf2588bc016a27130597bb1}"
HX_SOURCE_DIR="${CIUKIOS_HX_SOURCE_DIR:-$ROOT_DIR/build/external/audio-compat/HX-$HX_COMMIT}"
HX_PATCH="${CIUKIOS_HX_PATCH:-$ROOT_DIR/patches/hdpmi-ciukios-xms-status.patch}"
HXDEV_URL="${CIUKIOS_HXDEV_URL:-https://github.com/Baron-von-Riedesel/HX/releases/download/v2.23/HXDEV223.zip}"
HXDEV_SHA256="67f3790056410e984161dc1ff93b4c2907037f1029e5d4a5c8073a6e7d8a9814"
JWASM_REPOSITORY="${CIUKIOS_JWASM_REPOSITORY:-https://github.com/Baron-von-Riedesel/JWasm.git}"
JWASM_COMMIT="${CIUKIOS_JWASM_COMMIT:-7f6f32e78b79565d40bcce496756aadd1ff66900}"
JWASM_DIR="${CIUKIOS_JWASM_DIR:-$ROOT_DIR/build/external/JWasm}"
JWLINK_REPOSITORY="${CIUKIOS_JWLINK_REPOSITORY:-https://github.com/Baron-von-Riedesel/JWlink.git}"
JWLINK_COMMIT="${CIUKIOS_JWLINK_COMMIT:-cb7df1288619f7d3bf0290566bbe0b643fc05fd1}"
JWLINK_DIR="${CIUKIOS_JWLINK_DIR:-$ROOT_DIR/build/external/JWlink}"
OUTPUT_DIR="${CIUKIOS_SBEMU_OUTPUT_DIR:-$ROOT_DIR/build/external/audio-compat/output}"
WATCOM_ROOT="${WATCOM:-/opt/watcom}"

for command_name in curl gcc git make sha256sum unzip wine; do
	command -v "$command_name" >/dev/null 2>&1 \
		|| { echo "[build-hdpmi] ERROR: missing command: $command_name" >&2; exit 1; }
done
[[ -s "$HX_PATCH" ]] \
	|| { echo "[build-hdpmi] ERROR: missing CiukiOS HDPMI patch: $HX_PATCH" >&2; exit 1; }

prepare_checkout() {
	local repository="$1"
	local commit="$2"
	local destination="$3"
	local label="$4"
	if [[ ! -d "$destination/.git" ]]; then
		if [[ -e "$destination" ]]; then
			echo "[build-hdpmi] ERROR: $label destination is not a git checkout: $destination" >&2
			exit 1
		fi
		mkdir -p "$(dirname "$destination")"
		git clone --filter=blob:none "$repository" "$destination"
	fi
	if [[ "$(git -C "$destination" rev-parse HEAD)" != "$commit" ]]; then
		if [[ -n "$(git -C "$destination" status --porcelain)" ]]; then
			echo "[build-hdpmi] ERROR: refusing to replace modified $label checkout: $destination" >&2
			exit 1
		fi
		git -C "$destination" fetch --depth 1 origin "$commit"
		git -C "$destination" switch --detach "$commit"
	fi
}

prepare_checkout "$JWASM_REPOSITORY" "$JWASM_COMMIT" "$JWASM_DIR" JWasm
prepare_checkout "$JWLINK_REPOSITORY" "$JWLINK_COMMIT" "$JWLINK_DIR" JWlink
prepare_checkout "$HX_REPOSITORY" "$HX_COMMIT" "$HX_SOURCE_DIR" HX

if git -C "$HX_SOURCE_DIR" apply --reverse --check --ignore-space-change --ignore-whitespace "$HX_PATCH" >/dev/null 2>&1; then
	echo "[build-hdpmi] CiukiOS XMS/status patch already applied"
elif git -C "$HX_SOURCE_DIR" apply --check --ignore-space-change --ignore-whitespace "$HX_PATCH" >/dev/null 2>&1; then
	git -C "$HX_SOURCE_DIR" apply --ignore-space-change --ignore-whitespace "$HX_PATCH"
else
	echo "[build-hdpmi] ERROR: HX source is neither pristine nor exactly CiukiOS-patched" >&2
	git -C "$HX_SOURCE_DIR" status --short >&2 || true
	exit 1
fi

make -C "$JWASM_DIR" -f GccUnix.mak DEBUG=0 -j"${CIUKIOS_BUILD_JOBS:-2}"
make -C "$JWLINK_DIR" -f GccUnix.mak DEBUG=0 -j"${CIUKIOS_BUILD_JOBS:-2}"
JWASM="$JWASM_DIR/build/GccUnixR/jwasm"
JWLINK="$JWLINK_DIR/build/jwlinkLR/jwlink"
[[ -x "$JWASM" && -x "$JWLINK" ]] \
	|| { echo "[build-hdpmi] ERROR: host JWasm/JWlink build failed" >&2; exit 1; }

WLIB=""
for candidate in "$WATCOM_ROOT/binl64/wlib" "$WATCOM_ROOT/binl/wlib"; do
	if [[ -x "$candidate" ]]; then
		WLIB="$candidate"
		break
	fi
done
[[ -n "$WLIB" ]] \
	|| { echo "[build-hdpmi] ERROR: OpenWatcom wlib not found under $WATCOM_ROOT" >&2; exit 1; }

# HX was authored for a case-insensitive filesystem. Create lowercase include
# aliases inside the ignored checkout so JWasm resolves the original names on
# Linux without changing upstream source files.
for include_dir in "$HX_SOURCE_DIR/Include" "$HX_SOURCE_DIR/Src/HDPMI"; do
	while IFS= read -r -d '' include_path; do
		include_name="$(basename "$include_path")"
		lower_name="$(printf '%s' "$include_name" | tr '[:upper:]' '[:lower:]')"
		if [[ "$include_name" != "$lower_name" ]]; then
			ln -sfn "$include_name" "$(dirname "$include_path")/$lower_name"
		fi
	done < <(find "$include_dir" -maxdepth 1 -type f -iname '*.inc' -print0)
done

HDPMI_DIR="$HX_SOURCE_DIR/Src/HDPMI"
BUILD_DIR="$HDPMI_DIR/ciukios-release"
TOOLS_DIR="$HX_SOURCE_DIR/ciukios-tools"
mkdir -p "$BUILD_DIR" "$OUTPUT_DIR" "$TOOLS_DIR"
modules=(
	HDPMI A20GATE CLIENTS EXCEPT HEAP HELPERS I2FHDPMI I31DEB I31DOS
	I31FPU I31INT I31MEM I31SEL I31SWT INIT INT13API INT21API INT2FAPI
	INT2XAPI INT31API INT33API INT41API INTXXAPI MOVEHIGH PAGEMGR PUTCHR
	PUTCHRR SWITCH VXD
)

if [[ ! -s "$TOOLS_DIR/EDITPE.EXE" || ! -s "$TOOLS_DIR/PESTUB.EXE" ]]; then
	hxdev_archive="$TOOLS_DIR/HXDEV223.zip"
	curl --fail --location --retry 3 --connect-timeout 20 \
		--output "$hxdev_archive" "$HXDEV_URL"
	actual_hxdev_sha="$(sha256sum "$hxdev_archive" | awk '{print $1}')"
	[[ "$actual_hxdev_sha" == "$HXDEV_SHA256" ]] \
		|| { echo "[build-hdpmi] ERROR: HXDEV223 SHA-256 mismatch" >&2; exit 1; }
	unzip -j -o "$hxdev_archive" 'BIN/EDITPE.EXE' 'BIN/PESTUB.EXE' \
		-d "$TOOLS_DIR" >/dev/null
fi

build_hdpmi_iopl0() {
	local bits="$1"
	local name="HDPMI${bits}"
	local variant_dir="$BUILD_DIR/$name"
	local module
	local library_args=()
	mkdir -p "$variant_dir"

	echo "[build-hdpmi] assembling patched ${name}I (official HDPMI 3.24)"
	pushd "$HDPMI_DIR" >/dev/null
	for module in "${modules[@]}"; do
		"$JWASM" -nologo -c -Cp -Sg \
			"-D?32BIT=$([[ "$bits" == 32 ]] && echo 1 || echo 0)" \
			'-D?PMIOPL=0' '-D?PE' '-D?WDEB386=1' '-D?JHDPMI=1' \
			'-D?EMUDRxRD=1' '-D?EMUDRxWR=1' \
			-I../../Include -Fl"$variant_dir/$module.lst" \
			-Fo"$variant_dir/$module.obj" "$module.ASM"
	done
	popd >/dev/null

	for module in "${modules[@]:1}"; do
		library_args+=("+$module.obj")
	done
	pushd "$variant_dir" >/dev/null
	"$WLIB" -q -b -n "$name.lib" "${library_args[@]}"
	"$JWLINK" format win pe hx ru native file HDPMI.obj name "$name.TMP" \
		lib "$name.lib" op q,map="${name}I.MAP",nodosseg,stack=0,offset=0,align=0x100

	# This is the exact upstream HDPMI32I/HDPMI16I post-link sequence: move
	# the resident 16-bit section to RVA 0, extract it as the MZ stub, remove
	# the resident and relocation sections from the PE payload, then attach it.
	WINEDEBUG=-all wine "$TOOLS_DIR/EDITPE.EXE" -q a 3=0 \
		"$name.TMP" "${name}I.EXE"
	WINEDEBUG=-all wine "$TOOLS_DIR/EDITPE.EXE" -q x 3 /m \
		"${name}I.EXE" stub.bin
	WINEDEBUG=-all wine "$TOOLS_DIR/EDITPE.EXE" -q d 3 \
		"${name}I.EXE" "${name}I.EXE"
	WINEDEBUG=-all wine "$TOOLS_DIR/EDITPE.EXE" -q d 3 \
		"${name}I.EXE" "${name}I.EXE"
	WINEDEBUG=-all wine "$TOOLS_DIR/PESTUB.EXE" -q -n \
		"${name}I.EXE" stub.bin
	cp -- "${name}I.EXE" "$OUTPUT_DIR/${name}I.EXE"
	popd >/dev/null
}

build_hdpmi_iopl0 32
build_hdpmi_iopl0 16

cp -- "$HX_SOURCE_DIR/HXsrc.txt" "$OUTPUT_DIR/HDPMI.TXT"
[[ -s "$OUTPUT_DIR/HDPMI32I.EXE" && -s "$OUTPUT_DIR/HDPMI16I.EXE" \
	&& -s "$OUTPUT_DIR/HDPMI.TXT" ]] \
	|| { echo "[build-hdpmi] ERROR: incomplete output" >&2; exit 1; }
echo "[build-hdpmi] ready: official HDPMI 3.24 IOPL=0 hosts"
sha256sum "$OUTPUT_DIR/HDPMI32I.EXE" "$OUTPUT_DIR/HDPMI16I.EXE"
