#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DO_BUILD="${DO_BUILD:-1}"
DOOMVAN_MEMORY_REQUIRED="${DOOMVAN_MEMORY_REQUIRED:-1}"
IMG="${IMG:-build/full/ciukios-full.img}"
TEST_IMG="${TEST_IMG:-build/full/ciukios-full-doomvan-memory.img}"
SERIAL_LOG="${SERIAL_LOG:-build/full/qemu-full-doomvan-memory.serial.log}"
STDERR_LOG="${STDERR_LOG:-build/full/qemu-full-doomvan-memory.stderr.log}"
MONITOR_LOG="${MONITOR_LOG:-build/full/qemu-full-doomvan-memory.monitor.log}"
MON_SOCK="${MON_SOCK:-/tmp/ciukios-doomvan-memory.$$.monitor.sock}"
TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-90}"
NORMALIZER="scripts/serial_log_normalize.py"
SB16_CFG="build/full/obj/doomsb-default.cfg"
QEMU_PID=0
BASE_HASH_BEFORE=""

usage() {
    cat <<'TXT'
Usage: scripts/qemu_test_full_doomvan_memory.sh [--no-build]

Runs doom-vanille from an isolated full-image fixture and requires DOS/4GW,
the protected-mode heap and ST_Init to pass the former 256 KiB low-DOS
allocation failure. Set DOOMVAN_MEMORY_REQUIRED=0 to skip when the private
local payload is unavailable.
TXT
}

fail() {
    echo "[doomvan-memory] FAIL $*" >&2
    if ! base_image_unchanged; then
        echo "[doomvan-memory] FAIL BASE_IMAGE_MUTATED: $IMG changed during isolated lane" >&2
    fi
    if [[ -s "$SERIAL_LOG" ]]; then
        if [[ -x "$NORMALIZER" ]]; then
            "$NORMALIZER" "$SERIAL_LOG" | tail -n 120 >&2 || true
        else
            tail -n 120 "$SERIAL_LOG" >&2 || true
        fi
    fi
    exit 1
}

base_image_unchanged() {
    local current_hash

    [[ -n "$BASE_HASH_BEFORE" ]] || return 0
    [[ -f "$IMG" && ! -L "$IMG" ]] || return 1
    current_hash="$(sha256sum "$IMG" | awk '{print $1}')"
    [[ "$current_hash" == "$BASE_HASH_BEFORE" ]]
}

hmp() {
    local command="$1"
    echo "[HMP] $command" >> "$MONITOR_LOG"
    printf '%s\n' "$command" | socat - UNIX-CONNECT:"$MON_SOCK" >> "$MONITOR_LOG" 2>&1
}

cleanup() {
    if [[ "$QEMU_PID" -ne 0 ]] && kill -0 "$QEMU_PID" >/dev/null 2>&1; then
        if [[ -S "$MON_SOCK" ]]; then
            hmp quit >/dev/null 2>&1 || true
        fi
        kill "$QEMU_PID" >/dev/null 2>&1 || true
        wait "$QEMU_PID" >/dev/null 2>&1 || true
    fi
    case "$MON_SOCK" in
        /tmp/ciukios-doomvan-memory.*.monitor.sock)
            [[ "${MON_SOCK#/tmp/}" != */* ]] || return 1
            ;;
        *)
            return 1
            ;;
    esac
    if [[ -S "$MON_SOCK" && ! -L "$MON_SOCK" ]]; then
        rm -f -- "$MON_SOCK"
    elif [[ -e "$MON_SOCK" || -L "$MON_SOCK" ]]; then
        return 1
    fi
    return 0
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_socket() {
    local remaining=80
    while (( remaining > 0 )); do
        [[ -S "$MON_SOCK" ]] && return 0
        sleep 0.25
        remaining=$((remaining - 1))
    done
    return 1
}

normalized_log_has() {
    local marker="$1"
    if [[ -x "$NORMALIZER" ]]; then
        "$NORMALIZER" "$SERIAL_LOG" 2>/dev/null | grep -aFq "$marker"
    else
        grep -aFq "$marker" "$SERIAL_LOG"
    fi
}

validate_log_output_path() {
    local path="$1"

    case "$path" in
        build/full/*doomvan-memory*.log)
            [[ "${path#build/full/}" != */* ]] || return 1
            ;;
        *)
            return 1
            ;;
    esac
    if [[ -e "$path" || -L "$path" ]]; then
        [[ -f "$path" && ! -L "$path" ]] || return 1
    fi
}

wait_for_marker() {
    local marker="$1"
    local timeout="$2"
    local start now
    start="$(date +%s)"
    while true; do
        if [[ -s "$SERIAL_LOG" ]] && normalized_log_has "$marker"; then
            return 0
        fi
        if [[ "$QEMU_PID" -ne 0 ]] && ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
            return 1
        fi
        now="$(date +%s)"
        (( now - start < timeout )) || return 1
        sleep 0.25
    done
}

send_key() {
    hmp "sendkey $1" >/dev/null
    sleep 0.04
}

send_text_and_enter() {
    local input="$1"
    local index char key
    for ((index=0; index<${#input}; index++)); do
        char="${input:index:1}"
        if [[ "$char" == ' ' ]]; then
            key=spc
        elif [[ "$char" == '.' ]]; then
            key=dot
        elif [[ "$char" == $'\\' ]]; then
            key=backslash
        elif [[ "$char" == '-' ]]; then
            key=minus
        elif [[ "$char" =~ [A-Z] ]]; then
            key="shift-$(printf '%s' "$char" | tr 'A-Z' 'a-z')"
        elif [[ "$char" =~ [a-z0-9] ]]; then
            key="$char"
        else
            continue
        fi
        send_key "$key"
    done
    sleep 0.3
    send_key ret
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)
            DO_BUILD=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[doomvan-memory] ERROR unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$DO_BUILD" == "1" ]]; then
    bash scripts/build_full.sh
fi

[[ -f "$IMG" ]] || fail "missing image: $IMG"
[[ ! -L "$IMG" ]] || fail "refusing symlink source image: $IMG"
if ! mdir -i "$IMG" ::APPS/DOOMVAN/PCDOOM.EXE >/dev/null 2>&1; then
    if [[ "$DOOMVAN_MEMORY_REQUIRED" == "1" ]]; then
        fail "PCDOOM.EXE is not packaged in ::APPS/DOOMVAN"
    fi
    echo "[doomvan-memory] SKIP optional doom-vanille payload is unavailable"
    exit 0
fi
[[ -f "$SB16_CFG" ]] || fail "missing SB16 fixture: $SB16_CFG"
[[ -x "$NORMALIZER" ]] || fail "missing serial normalizer: $NORMALIZER"
command -v qemu-system-i386 >/dev/null 2>&1 || fail "qemu-system-i386 is unavailable"
command -v socat >/dev/null 2>&1 || fail "socat is unavailable"
command -v mcopy >/dev/null 2>&1 || fail "mcopy is unavailable"
command -v mtype >/dev/null 2>&1 || fail "mtype is unavailable"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is unavailable"
command -v realpath >/dev/null 2>&1 || fail "realpath is unavailable"
[[ "$TEST_IMG" != "$IMG" ]] || fail "TEST_IMG must not overwrite the source image"
case "$TEST_IMG" in
    build/full/*doomvan-memory*.img)
        [[ "${TEST_IMG#build/full/}" != */* ]] \
            || fail "refusing nested TEST_IMG path: $TEST_IMG"
        ;;
    *)
        fail "unsafe TEST_IMG path (expected build/full/*doomvan-memory*.img): $TEST_IMG"
        ;;
esac
case "$MON_SOCK" in
    /tmp/ciukios-doomvan-memory.*.monitor.sock)
        [[ "${MON_SOCK#/tmp/}" != */* ]] \
            || fail "refusing nested MON_SOCK path: $MON_SOCK"
        ;;
    *)
        fail "unsafe MON_SOCK path: $MON_SOCK"
        ;;
esac
if [[ -e "$MON_SOCK" || -L "$MON_SOCK" ]]; then
    [[ -S "$MON_SOCK" && ! -L "$MON_SOCK" ]] \
        || fail "refusing non-socket or symlink MON_SOCK: $MON_SOCK"
fi
for output_path in "$SERIAL_LOG" "$STDERR_LOG" "$MONITOR_LOG"; do
    validate_log_output_path "$output_path" \
        || fail "unsafe log output path: $output_path"
done
[[ "$SERIAL_LOG" != "$STDERR_LOG" \
    && "$SERIAL_LOG" != "$MONITOR_LOG" \
    && "$STDERR_LOG" != "$MONITOR_LOG" ]] \
    || fail "serial, stderr and monitor logs must use distinct paths"

IMG_REAL="$(realpath -e -- "$IMG")" \
    || fail "cannot resolve source image: $IMG"
TEST_IMG_REAL="$(realpath -m -- "$TEST_IMG")" \
    || fail "cannot resolve fixture image: $TEST_IMG"
[[ "$TEST_IMG_REAL" != "$IMG_REAL" ]] \
    || fail "TEST_IMG resolves to the source image"
if [[ -e "$TEST_IMG" || -L "$TEST_IMG" ]]; then
    [[ -f "$TEST_IMG" && ! -L "$TEST_IMG" ]] \
        || fail "refusing non-regular or symlink TEST_IMG: $TEST_IMG"
    [[ ! "$TEST_IMG" -ef "$IMG" ]] \
        || fail "TEST_IMG aliases the source image"
    rm -f -- "$TEST_IMG"
fi
BASE_HASH_BEFORE="$(sha256sum "$IMG" | awk '{print $1}')"

# Force the exact path which previously failed in I_AllocLow(256000).
cp --reflink=auto --sparse=always "$IMG" "$TEST_IMG"
mcopy -o -i "$TEST_IMG" "$SB16_CFG" ::APPS/DOOMVAN/DEFAULT.CFG
if ! mtype -i "$TEST_IMG" ::APPS/DOOMVAN/DEFAULT.CFG \
    | grep -Eq '^snd_sfxdevice[[:space:]]+3[[:space:]]*$'; then
    fail "SB16 configuration was not installed"
fi
base_image_unchanged || fail "source image changed during fixture preparation"

mkdir -p "$(dirname "$SERIAL_LOG")"
rm -f "$SERIAL_LOG" "$STDERR_LOG" "$MONITOR_LOG" "$MON_SOCK"

timeout "$TIMEOUT_SEC" qemu-system-i386 \
    -machine pc,vmport=off,pcspk-audiodev=snd0 \
    -cpu pentium3 \
    -m 128 \
    -drive "file=$TEST_IMG,format=raw,if=ide" \
    -boot c \
    -display none \
    -chardev "file,id=ser0,path=$SERIAL_LOG" \
    -serial chardev:ser0 \
    -monitor "unix:$MON_SOCK,server,nowait" \
    -audiodev none,id=snd0 \
    -device sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd0 \
    -no-reboot \
    -no-shutdown \
    >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!

wait_for_socket || fail "QEMU monitor socket did not appear"
wait_for_marker 'CiukiOS SHELL C:\APPS>' 30 \
    || fail "external shell prompt did not appear"

send_text_and_enter 'cd \APPS\DOOMVAN'
sleep 1
send_text_and_enter 'run PCDOOM.EXE -nomusic'

wait_for_marker 'DOS/4GW Protected Mode Run-time' 45 \
    || fail "DOS/4GW did not initialize"
wait_for_marker 'DPMI memory:' 30 \
    || fail "protected-mode heap did not initialize"
wait_for_marker 'ST_Init: Init status bar.' 45 \
    || fail "pcdoom did not finish startup after the low-DOS allocation"

if normalized_log_has 'I_AllocLow:'; then
    fail "256000-byte conventional-memory allocation still fails"
fi

sleep 2
if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
    fail "QEMU exited immediately after the startup marker"
fi
if ! hmp quit >/dev/null 2>&1; then
    fail "could not request a controlled QEMU shutdown"
fi

set +e
wait "$QEMU_PID"
QEMU_RC=$?
set -e
QEMU_PID=0
if [[ "$QEMU_RC" -ne 0 ]]; then
    fail "QEMU exited with status $QEMU_RC after controlled shutdown"
fi
base_image_unchanged || fail "source image changed while QEMU used the fixture"

echo "[doomvan-memory] PASS sb16_low_dos_256k"
