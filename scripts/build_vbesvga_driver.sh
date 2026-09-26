#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
: "${VBESVGA_OUTPUT_DIR:=$CIUKIOS_ROOT/build/external/video-compat/output}"

readonly VBESVGA_TAG="v1.0-beta4"
readonly VBESVGA_COMMIT="afaacaef6b7036e4d8a8b947244e2f4a6c209564"
readonly VBESVGA_RELEASE_SHA256="e4272c942b9330305af8c1388a1e56289913f4eb6dfcda4c4af1ba256e1a770f"
readonly VBESVGA_URL="https://github.com/PluMGMK/vbesvga.drv/releases/download/${VBESVGA_TAG}/vbesvga-release.zip"
readonly VBESVGA_REPO="https://github.com/PluMGMK/vbesvga.drv.git"

cache_dir="$CIUKIOS_ROOT/build/external/video-compat"
archive="$cache_dir/vbesvga-release-${VBESVGA_TAG}.zip"
source_dir="$cache_dir/vbesvga-${VBESVGA_COMMIT}"
extract_dir="$cache_dir/release-${VBESVGA_TAG}"
rebuilt_dir="$cache_dir/rebuilt-${VBESVGA_COMMIT}"

for tool in curl git sha256sum unzip cmp patch tr; do
	command -v "$tool" >/dev/null 2>&1 \
		|| { echo "[vbesvga] ERROR: required tool missing: $tool" >&2; exit 1; }
done

mkdir -p "$cache_dir" "$VBESVGA_OUTPUT_DIR"
if [[ ! -s "$archive" ]] \
	|| [[ "$(sha256sum "$archive" | awk '{print $1}')" != "$VBESVGA_RELEASE_SHA256" ]]; then
	echo "[vbesvga] fetching pinned ${VBESVGA_TAG} release"
	curl -fL --retry 3 --connect-timeout 20 -o "$archive.tmp" "$VBESVGA_URL"
	echo "$VBESVGA_RELEASE_SHA256  $archive.tmp" | sha256sum -c -
	mv -f "$archive.tmp" "$archive"
fi
echo "$VBESVGA_RELEASE_SHA256  $archive" | sha256sum -c - >/dev/null

if [[ ! -d "$source_dir/.git" ]]; then
	rm -rf -- "$source_dir"
	git clone --quiet "$VBESVGA_REPO" "$source_dir"
fi
git -C "$source_dir" fetch --quiet origin "$VBESVGA_COMMIT"
git -C "$source_dir" checkout --quiet --detach "$VBESVGA_COMMIT"
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$VBESVGA_COMMIT" ]] \
	|| { echo "[vbesvga] ERROR: source checkout is not the pinned commit" >&2; exit 1; }

rm -rf -- "$extract_dir" "$rebuilt_dir"
mkdir -p "$extract_dir" "$rebuilt_dir"
unzip -oq "$archive" -d "$extract_dir"

jwasm="${JWASM_BIN:-$CIUKIOS_ROOT/build/external/JWasm/build/GccUnixR/jwasm}"
if [[ ! -x "$jwasm" ]]; then
	jwasm="$(command -v jwasm || true)"
fi
[[ -n "$jwasm" && -x "$jwasm" ]] \
	|| { echo "[vbesvga] ERROR: JWasm is required to reproduce the DOS tools" >&2; exit 1; }

# Recompile every standalone component whose complete toolchain is open and
# available here.  The resulting binaries must byte-match the release asset;
# this ties the pinned driver binaries to the exact source tag instead of
# trusting an unrelated archive with the same filenames.
"$jwasm" -bin -Fo "$rebuilt_dir/AUXSTACK.COM" "$source_dir/AUXSTACK.ASM" >/dev/null
"$jwasm" -bin -Fo "$rebuilt_dir/AUXCHECK.COM" "$source_dir/AUXCHECK.ASM" >/dev/null
"$jwasm" -bin -DVIDMODES_COMMIT=\'b6c68f7\' -Fo "$rebuilt_dir/VIDMODES.COM" "$source_dir/VIDMODES.ASM" >/dev/null
"$jwasm" -bin -DMODETEST_COMMIT=\'1698b39\' -Fo "$rebuilt_dir/MODETEST.COM" "$source_dir/MODETEST.ASM" >/dev/null
"$jwasm" -bin -DSETUP_COMMIT=\'adb994c\' -Fo "$rebuilt_dir/SETUP.EXE" "$source_dir/SETUP.ASM" >/dev/null

for rebuilt in AUXSTACK.COM AUXCHECK.COM VIDMODES.COM MODETEST.COM SETUP.EXE; do
	cmp -s "$rebuilt_dir/$rebuilt" "$extract_dir/$rebuilt" \
		|| { echo "[vbesvga] ERROR: source rebuild differs from release: $rebuilt" >&2; exit 1; }
done

# Verify the upstream build first, then build the reviewed modeset/bank error
# handling patch. Keep the pinned checkout and the Win16 driver unchanged.
patched_helpers="$cache_dir/patched-helpers-${VBESVGA_COMMIT}"
mkdir -p "$patched_helpers"
tr -d '\r' < "$source_dir/MODETEST.ASM" > "$patched_helpers/MODETEST.ASM"
patch --batch --forward -d "$patched_helpers" -p1 \
  < "$CIUKIOS_ROOT/patches/vbesvga-modetest-bios-status.patch"
"$jwasm" -bin -DMODETEST_COMMIT=\'1698b39-ciukios\' \
  -Fo "$rebuilt_dir/MODETEST.COM" "$patched_helpers/MODETEST.ASM" >/dev/null

for required in VBESVGA.DRV VDDVBE.386 VBEVMDIB.3GR VBESVGA.TXT OEMSETUP.INF; do
	[[ -s "$extract_dir/$required" ]] \
		|| { echo "[vbesvga] ERROR: release is missing $required" >&2; exit 1; }
done

cp -f "$extract_dir/VBESVGA.DRV" "$extract_dir/VDDVBE.386" \
	"$extract_dir/VBEVMDIB.3GR" "$extract_dir/VBESVGA.TXT" \
	"$extract_dir/OEMSETUP.INF" "$VBESVGA_OUTPUT_DIR/"
cp -f "$rebuilt_dir/"* "$VBESVGA_OUTPUT_DIR/"

{
	echo "Modern Generic SVGA for Windows 3.1"
	echo "upstream=$VBESVGA_REPO"
	echo "tag=$VBESVGA_TAG"
	echo "commit=$VBESVGA_COMMIT"
	echo "release_sha256=$VBESVGA_RELEASE_SHA256"
	echo "helpers=upstream-byte-matched-before-modetest-bios-status-patch"
	echo "modetest_patch_sha256=$(sha256sum "$CIUKIOS_ROOT/patches/vbesvga-modetest-bios-status.patch" | awk '{print $1}')"
	echo "drivers=pinned-upstream-build-requiring-Win16-DDK"
} > "$VBESVGA_OUTPUT_DIR/SOURCE.TXT"

echo "[vbesvga] verified source/release driver payload: $VBESVGA_OUTPUT_DIR"
