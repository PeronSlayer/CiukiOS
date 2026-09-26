#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SBEMU_REPOSITORY="${CIUKIOS_SBEMU_REPOSITORY:-https://github.com/crazii/SBEMU.git}"
SBEMU_COMMIT="${CIUKIOS_SBEMU_COMMIT:-823fa75e06f5bcf4de16879dcc4fbfd59c9a0b6b}"
SBEMU_SOURCE_DIR="${CIUKIOS_SBEMU_SOURCE_DIR:-$ROOT_DIR/build/external/audio-compat/SBEMU-$SBEMU_COMMIT}"
SBEMU_PATCH="${CIUKIOS_SBEMU_PATCH:-$ROOT_DIR/patches/sbemu-ich-32-slot-ring.patch}"
SBEMU_OUTPUT_DIR="${CIUKIOS_SBEMU_OUTPUT_DIR:-$ROOT_DIR/build/external/audio-compat/output}"
DJGPP_ROOT="${CIUKIOS_DJGPP_ROOT:-$ROOT_DIR/build/external/djgpp}"
HDPMI_BUILD_SCRIPT="${CIUKIOS_HDPMI_BUILD_SCRIPT:-$ROOT_DIR/scripts/build_hdpmi_host.sh}"

for command_name in git make sha256sum; do
	command -v "$command_name" >/dev/null 2>&1 \
		|| { echo "[build-sbemu] ERROR: missing command: $command_name" >&2; exit 1; }
done
[[ -s "$SBEMU_PATCH" ]] \
	|| { echo "[build-sbemu] ERROR: missing CiukiOS ICH patch: $SBEMU_PATCH" >&2; exit 1; }
[[ -x "$DJGPP_ROOT/bin/i586-pc-msdosdjgpp-gcc" ]] \
	|| { echo "[build-sbemu] ERROR: DJGPP cross-compiler not found under $DJGPP_ROOT" >&2; exit 1; }
[[ -x "$HDPMI_BUILD_SCRIPT" ]] \
	|| { echo "[build-sbemu] ERROR: missing HDPMI build script: $HDPMI_BUILD_SCRIPT" >&2; exit 1; }

if [[ ! -d "$SBEMU_SOURCE_DIR/.git" ]]; then
	if [[ -e "$SBEMU_SOURCE_DIR" ]]; then
		echo "[build-sbemu] ERROR: source destination exists but is not a git checkout: $SBEMU_SOURCE_DIR" >&2
		exit 1
	fi
	mkdir -p "$(dirname "$SBEMU_SOURCE_DIR")"
	git clone --filter=blob:none "$SBEMU_REPOSITORY" "$SBEMU_SOURCE_DIR"
fi

actual_commit="$(git -C "$SBEMU_SOURCE_DIR" rev-parse HEAD)"
if [[ "$actual_commit" != "$SBEMU_COMMIT" ]]; then
	if [[ -n "$(git -C "$SBEMU_SOURCE_DIR" status --porcelain)" ]]; then
		echo "[build-sbemu] ERROR: refusing to replace a modified checkout at $SBEMU_SOURCE_DIR" >&2
		exit 1
	fi
	git -C "$SBEMU_SOURCE_DIR" fetch --depth 1 origin "$SBEMU_COMMIT"
	git -C "$SBEMU_SOURCE_DIR" switch --detach "$SBEMU_COMMIT"
fi

if git -C "$SBEMU_SOURCE_DIR" apply --reverse --check "$SBEMU_PATCH" >/dev/null 2>&1; then
	echo "[build-sbemu] CiukiOS ICH patch already applied"
elif git -C "$SBEMU_SOURCE_DIR" apply --check "$SBEMU_PATCH" >/dev/null 2>&1; then
	git -C "$SBEMU_SOURCE_DIR" apply "$SBEMU_PATCH"
else
	echo "[build-sbemu] ERROR: source is neither pristine nor exactly CiukiOS-patched" >&2
	git -C "$SBEMU_SOURCE_DIR" status --short >&2 || true
	exit 1
fi

echo "[build-sbemu] compiling SBEMU at $SBEMU_COMMIT"
make -C "$SBEMU_SOURCE_DIR" clean
PATH="$DJGPP_ROOT/bin:/usr/bin:/bin" make -C "$SBEMU_SOURCE_DIR" DEBUG=0 CIUKIOS=1 -j"${CIUKIOS_BUILD_JOBS:-2}"
[[ -s "$SBEMU_SOURCE_DIR/output/sbemu.exe" ]] \
	|| { echo "[build-sbemu] ERROR: SBEMU build produced no executable" >&2; exit 1; }

mkdir -p "$SBEMU_OUTPUT_DIR"
CIUKIOS_SBEMU_OUTPUT_DIR="$SBEMU_OUTPUT_DIR" bash "$HDPMI_BUILD_SCRIPT"
cp -- "$SBEMU_SOURCE_DIR/output/sbemu.exe" "$SBEMU_OUTPUT_DIR/SBEMU.EXE"
cp -- "$SBEMU_SOURCE_DIR/COPYING" "$SBEMU_OUTPUT_DIR/SBEMU.GPL"
cp -- "$SBEMU_SOURCE_DIR/README.txt" "$SBEMU_OUTPUT_DIR/SBEMU.TXT"

for output_file in SBEMU.EXE HDPMI32I.EXE HDPMI.TXT SBEMU.GPL SBEMU.TXT; do
	[[ -s "$SBEMU_OUTPUT_DIR/$output_file" ]] \
		|| { echo "[build-sbemu] ERROR: incomplete output: $output_file" >&2; exit 1; }
done

echo "[build-sbemu] ready: $SBEMU_OUTPUT_DIR"
sha256sum "$SBEMU_OUTPUT_DIR/SBEMU.EXE" "$SBEMU_OUTPUT_DIR/HDPMI32I.EXE"
