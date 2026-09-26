#!/usr/bin/env bash
set -euo pipefail

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$CIUKIOS_ROOT"

DO_BUILD=1
if [[ "${1:-}" == "--no-build" ]]; then
	DO_BUILD=0
fi
if (( DO_BUILD )); then
	bash scripts/build_full.sh
fi

IMG="build/full/ciukios-full.img"
PREFIX="build/full/qemu-full-ac97-audio"
WAV_FILE="${PREFIX}.wav"
SERIAL_LOG="${PREFIX}.serial.log"
NORMALIZED_LOG="${PREFIX}.normalized.log"
STDERR_LOG="${PREFIX}.stderr.log"
MON_SOCK="/tmp/ciukios-ac97-audio.$$.monitor.sock"
qemu_pid=""
AUDIO_TEST_BACKEND="${CIUKIOS_AUDIO_TEST_BACKEND:-ac97}"
QEMU_MACHINE_ARG="pc,vmport=off"
QEMU_AUDIO_DEVICE_ARGS=()
EXPECTED_BACKEND='Intel-ICH-AC97'
case "$AUDIO_TEST_BACKEND" in
	ac97)
		QEMU_AUDIO_DEVICE_ARGS=(-device "AC97,audiodev=snd0")
		;;
	pcspeaker)
		QEMU_MACHINE_ARG="pc,vmport=off,pcspk-audiodev=snd0"
		EXPECTED_BACKEND='PC-speaker'
		;;
	*)
		echo "[ac97-audio] FAIL unknown CIUKIOS_AUDIO_TEST_BACKEND=$AUDIO_TEST_BACKEND" >&2
		exit 2
		;;
esac

cleanup() {
	if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" >/dev/null 2>&1; then
		kill "$qemu_pid" >/dev/null 2>&1 || true
		wait "$qemu_pid" >/dev/null 2>&1 || true
	fi
	rm -f "$MON_SOCK"
}
trap cleanup EXIT

for command_name in qemu-system-i386 socat python3 mdir; do
	command -v "$command_name" >/dev/null 2>&1 \
		|| { echo "[ac97-audio] FAIL missing command: $command_name" >&2; exit 1; }
done
for image_path in ::SYSTEM/DRIVERS/AUDIO.COM ::SYSTEM/DRIVERS/AC97INIT.COM; do
	mdir -i "$IMG" "$image_path" >/dev/null 2>&1 \
		|| { echo "[ac97-audio] FAIL missing payload: $image_path" >&2; exit 1; }
done

rm -f "$WAV_FILE" "$SERIAL_LOG" "$NORMALIZED_LOG" "$STDERR_LOG" "$MON_SOCK"
qemu-system-i386 \
	-machine "$QEMU_MACHINE_ARG" \
	-cpu pentium3 -m 128 \
	-drive "file=$IMG,format=raw,if=ide,snapshot=on" \
	-boot c -display none \
	-audiodev "wav,id=snd0,path=$WAV_FILE" \
	"${QEMU_AUDIO_DEVICE_ARGS[@]}" \
	-serial "file:$SERIAL_LOG" \
	-monitor "unix:$MON_SOCK,server,nowait" \
	-no-reboot -no-shutdown \
	>/dev/null 2>"$STDERR_LOG" &
qemu_pid=$!

wait_for_pattern() {
	local pattern="$1" timeout_sec="$2" elapsed=0
	while (( elapsed < timeout_sec * 5 )); do
		if [[ -f "$SERIAL_LOG" ]]; then
			python3 scripts/serial_log_normalize.py "$SERIAL_LOG" > "$NORMALIZED_LOG" 2>/dev/null || true
			if grep -aEq "$pattern" "$NORMALIZED_LOG"; then
				return 0
			fi
		fi
		kill -0 "$qemu_pid" >/dev/null 2>&1 || return 1
		sleep 0.2
		((elapsed += 1))
	done
	return 1
}

send_key() {
	printf 'sendkey %s\n' "$1" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null
	sleep 0.04
}

send_text() {
	local input="$1" char key index
	for ((index=0; index<${#input}; index++)); do
		char="${input:index:1}"
		case "$char" in
			' ') key=spc ;;
			'\') key=backslash ;;
			'.') key=dot ;;
			[A-Z]) key="shift-$(printf '%s' "$char" | tr 'A-Z' 'a-z')" ;;
			[a-z0-9]) key="$char" ;;
			*) continue ;;
		esac
		send_key "$key"
	done
}

wait_for_pattern 'CiukiOS([[:space:]]+SHELL)?[[:space:]]+C:\\APPS>' 60 \
	|| { echo '[ac97-audio] FAIL shell prompt missing' >&2; exit 1; }
send_text 'run \SYSTEM\DRIVERS\AUDIO.COM'
send_key ret
wait_for_pattern "\\[AUDIO\\][[:space:]]+backend=${EXPECTED_BACKEND}" 30 \
	|| { echo "[ac97-audio] FAIL $EXPECTED_BACKEND backend marker missing" >&2; exit 1; }

cleanup
qemu_pid=""
python3 scripts/analyze_audio_wav.py "$WAV_FILE" \
	--label "$AUDIO_TEST_BACKEND-audio" --min-bytes 10000 --min-unique 4 --min-changes 100
echo "[ac97-audio] PASS backend=$EXPECTED_BACKEND auto-detection and playback"
