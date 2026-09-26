#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VSBHDA_REPOSITORY="${CIUKIOS_VSBHDA_REPOSITORY:-https://github.com/Baron-von-Riedesel/VSBHDA.git}"
VSBHDA_COMMIT="${CIUKIOS_VSBHDA_COMMIT:-75fa4bbfea70cbcc0c40d1212f04952ff8abbf16}"
VSBHDA_SOURCE_DIR="${CIUKIOS_VSBHDA_SOURCE_DIR:-$ROOT_DIR/build/external/audio-compat/VSBHDA-$VSBHDA_COMMIT}"
VSBHDA_PATCH="${CIUKIOS_VSBHDA_PATCH:-$ROOT_DIR/patches/vsbhda-ciukios-transient-run.patch}"
VSBHDA_ICH_PATCH="$ROOT_DIR/patches/vsbhda-ciukios-ich-codec.patch"
VSBHDA_RELEASE_URL="${CIUKIOS_VSBHDA_RELEASE_URL:-https://github.com/Baron-von-Riedesel/VSBHDA/releases/download/v2.0/vsbhda20.zip}"
VSBHDA_RELEASE_SHA256="9b937c3ccef55d70946aa0f9c0e391f8c76634a419cbc74e77e72c0a3d434c09"
OUTPUT_DIR="${CIUKIOS_SBEMU_OUTPUT_DIR:-$ROOT_DIR/build/external/audio-compat/output}"
WATCOM_ROOT="${WATCOM:-/opt/watcom}"
JWASM_DIR="${CIUKIOS_JWASM_DIR:-$ROOT_DIR/build/external/JWasm}"
JWLINK_DIR="${CIUKIOS_JWLINK_DIR:-$ROOT_DIR/build/external/JWlink}"
HDPMI_BUILD_SCRIPT="$ROOT_DIR/scripts/build_hdpmi_host.sh"

for command_name in curl git make sha256sum 7z; do
	command -v "$command_name" >/dev/null 2>&1 \
		|| { echo "[build-vsbhda] ERROR: missing command: $command_name" >&2; exit 1; }
done
[[ -s "$VSBHDA_PATCH" && -s "$VSBHDA_ICH_PATCH" && -x "$HDPMI_BUILD_SCRIPT" ]] \
	|| { echo "[build-vsbhda] ERROR: missing CiukiOS patch or HDPMI builder" >&2; exit 1; }

if [[ ! -d "$VSBHDA_SOURCE_DIR/.git" ]]; then
	if [[ -e "$VSBHDA_SOURCE_DIR" ]]; then
		echo "[build-vsbhda] ERROR: source destination is not a git checkout: $VSBHDA_SOURCE_DIR" >&2
		exit 1
	fi
	mkdir -p "$(dirname "$VSBHDA_SOURCE_DIR")"
	git clone --filter=blob:none "$VSBHDA_REPOSITORY" "$VSBHDA_SOURCE_DIR"
fi
if [[ "$(git -C "$VSBHDA_SOURCE_DIR" rev-parse HEAD)" != "$VSBHDA_COMMIT" ]]; then
	if [[ -n "$(git -C "$VSBHDA_SOURCE_DIR" status --porcelain)" ]]; then
		echo "[build-vsbhda] ERROR: refusing to replace modified checkout" >&2
		exit 1
	fi
	git -C "$VSBHDA_SOURCE_DIR" fetch --depth 1 origin "$VSBHDA_COMMIT"
	git -C "$VSBHDA_SOURCE_DIR" switch --detach "$VSBHDA_COMMIT"
fi
for source_patch in "$VSBHDA_PATCH" "$VSBHDA_ICH_PATCH"; do
	if git -C "$VSBHDA_SOURCE_DIR" apply --reverse --check --ignore-space-change --ignore-whitespace "$source_patch" >/dev/null 2>&1; then
		echo "[build-vsbhda] already applied: ${source_patch##*/}"
	elif git -C "$VSBHDA_SOURCE_DIR" apply --check --ignore-space-change --ignore-whitespace "$source_patch" >/dev/null 2>&1; then
		git -C "$VSBHDA_SOURCE_DIR" apply --ignore-space-change --ignore-whitespace "$source_patch"
	else
		echo "[build-vsbhda] ERROR: checkout does not match ${source_patch##*/}" >&2
		git -C "$VSBHDA_SOURCE_DIR" status --short >&2 || true
		exit 1
	fi
done

CIUKIOS_SBEMU_OUTPUT_DIR="$OUTPUT_DIR" bash "$HDPMI_BUILD_SCRIPT"
JWASM="$JWASM_DIR/build/GccUnixR/jwasm"
JWLINK="$JWLINK_DIR/build/jwlinkLR/jwlink"
[[ -x "$JWASM" && -x "$JWLINK" ]] \
	|| { echo "[build-vsbhda] ERROR: host JWasm/JWlink are unavailable" >&2; exit 1; }

echo "[build-vsbhda] compiling VSBHDA 2.0 with synchronous child cleanup"
make -C "$VSBHDA_SOURCE_DIR" -f Linux.mak clean >/dev/null 2>&1 || true
# The 16-bit EXE and DRV recipes invoke recursive builds that update the
# same OpenWatcom libraries. Serialize them to avoid corrupt/missing .lib
# files; the independent host-tool builds may still use CIUKIOS_BUILD_JOBS.
PATH="$(dirname "$JWASM"):$(dirname "$JWLINK"):$WATCOM_ROOT/binl:/usr/bin:/bin" \
	make -C "$VSBHDA_SOURCE_DIR" -f Linux.mak WATCOM="$WATCOM_ROOT" USEJWL=1 -j1

release_dir="$(mktemp -d)"
trap 'rm -rf -- "$release_dir"' EXIT
curl --fail --location --retry 3 --connect-timeout 20 \
	--output "$release_dir/vsbhda20.zip" "$VSBHDA_RELEASE_URL"
actual_release_sha="$(sha256sum "$release_dir/vsbhda20.zip" | awk '{print $1}')"
[[ "$actual_release_sha" == "$VSBHDA_RELEASE_SHA256" ]] \
	|| { echo "[build-vsbhda] ERROR: upstream release SHA-256 mismatch" >&2; exit 1; }
7z x -y -o"$release_dir/release" "$release_dir/vsbhda20.zip" >/dev/null

mkdir -p "$OUTPUT_DIR"
cp -- "$VSBHDA_SOURCE_DIR/ow/VSBHDA.EXE" "$OUTPUT_DIR/VSBHDA.EXE"
cp -- "$VSBHDA_SOURCE_DIR/ow16/VSBHDA16.EXE" "$OUTPUT_DIR/VSBHDA16.EXE"
cp -- "$VSBHDA_SOURCE_DIR/ow16/SNDCARD.DRV" "$OUTPUT_DIR/SNDCARD.DRV"
cp -- "$VSBHDA_SOURCE_DIR/vsbhda.txt" "$OUTPUT_DIR/VSBHDA.TXT"
cp -- "$VSBHDA_SOURCE_DIR/LICENSE" "$OUTPUT_DIR/VSBHDA.GPL"
cp -- "$release_dir/release/Win31/DOSX.EXE" "$OUTPUT_DIR/DOSXVS.EXE"
cp -- "$release_dir/release/Win31/DOSX.TXT" "$OUTPUT_DIR/DOSXVS.TXT"

for output_file in VSBHDA.EXE VSBHDA16.EXE SNDCARD.DRV VSBHDA.TXT VSBHDA.GPL \
	HDPMI32I.EXE HDPMI16I.EXE DOSXVS.EXE DOSXVS.TXT; do
	[[ -s "$OUTPUT_DIR/$output_file" ]] \
		|| { echo "[build-vsbhda] ERROR: incomplete output: $output_file" >&2; exit 1; }
done

echo "[build-vsbhda] ready: $OUTPUT_DIR"
sha256sum "$OUTPUT_DIR/VSBHDA.EXE" "$OUTPUT_DIR/VSBHDA16.EXE" \
	"$OUTPUT_DIR/SNDCARD.DRV" "$OUTPUT_DIR/HDPMI32I.EXE" "$OUTPUT_DIR/HDPMI16I.EXE"
