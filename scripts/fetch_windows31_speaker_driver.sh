#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

source_url="${CIUKIOS_WINDOWS31_SPEAKER_URL:-https://ftpmirror.your.org/pub/misc/ftp.microsoft.com/Softlib/MSLFILES/SPEAK.EXE}"
expected_sha256="d1b8a357bc65b467b7a96e0edb76816e48705db1c62a0f7dd7eb744196e317e4"
output_dir="${CIUKIOS_WINDOWS31_SPEAKER_DIR:-$CIUKIOS_ROOT/build/external/windows31-speaker}"
temp_dir="$(mktemp -d)"

cleanup() {
	rm -rf -- "$temp_dir"
}
trap cleanup EXIT

for command_name in curl sha256sum 7z; do
	command -v "$command_name" >/dev/null 2>&1 \
		|| { echo "[win31-speaker] ERROR: missing command: $command_name" >&2; exit 1; }
done

archive="$temp_dir/SPEAK.EXE"
expanded="$temp_dir/expanded"
mkdir -p "$expanded"

echo "[win31-speaker] fetching Microsoft's Windows 3.1 PC-speaker driver"
curl --fail --location --retry 3 --connect-timeout 20 \
	--output "$archive" "$source_url"

actual_sha256="$(sha256sum "$archive" | awk '{print $1}')"
[[ "$actual_sha256" == "$expected_sha256" ]] || {
	echo "[win31-speaker] ERROR: SPEAK.EXE SHA-256 mismatch" >&2
	echo "[win31-speaker] expected: $expected_sha256" >&2
	echo "[win31-speaker] actual:   $actual_sha256" >&2
	exit 1
}

7z x -y -o"$expanded" "$archive" >/dev/null

for required_file in SPEAKER.DRV OEMSETUP.INF LICENSE.TXT SPEAKER.TXT AUDIO.TXT; do
	[[ -s "$expanded/$required_file" ]] \
		|| { echo "[win31-speaker] ERROR: archive is missing $required_file" >&2; exit 1; }
done
[[ "$(stat -c%s "$expanded/SPEAKER.DRV")" -eq 7088 ]] \
	|| { echo "[win31-speaker] ERROR: unexpected SPEAKER.DRV size" >&2; exit 1; }

mkdir -p "$output_dir"
for packaged_file in SPEAKER.DRV OEMSETUP.INF LICENSE.TXT SPEAKER.TXT AUDIO.TXT; do
	cp -- "$expanded/$packaged_file" "$output_dir/$packaged_file"
done
cp -- "$archive" "$output_dir/SPEAK.EXE"

echo "[win31-speaker] verified output: $output_dir/SPEAKER.DRV (7088 bytes)"
echo "[win31-speaker] source SHA-256: $actual_sha256"
