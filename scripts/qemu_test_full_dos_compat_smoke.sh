#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
SERIAL_NORMALIZER="$ROOT_DIR/scripts/serial_log_normalize.py"

DO_BUILD=1
IMG="${IMG:-build/full/ciukios-full.img}"
PREFIX="build/full/qemu-full-dos-compat-smoke"
SERIAL_LOG="${PREFIX}.serial.log"
STRINGS_LOG="${PREFIX}.strings.log"
STDERR_LOG="${PREFIX}.stderr.log"
CMD_LOG="${PREFIX}.commands.log"
META_LOG="${PREFIX}.meta"
CIUKEDIT_MAIN_SCREENSHOT="${PREFIX}.ciukedit-main.ppm"
CIUKEDIT_HELP_SCREENSHOT="${PREFIX}.ciukedit-help.ppm"
CIUKEDIT_WARNING_SCREENSHOT="${PREFIX}.ciukedit-warning.ppm"
DOSNAV_BASELINE_SCREENSHOT="${PREFIX}.dosnav-baseline.ppm"
DOSNAV_NAV_HOME_SCREENSHOT="${PREFIX}.dosnav-nav-home.ppm"
DOSNAV_NAV_DOWN_SCREENSHOT="${PREFIX}.dosnav-nav-down.ppm"
DOSNAV_COLORS_SCREENSHOT="${PREFIX}.dosnav-colors.ppm"
DOSNAV_PALETTE_SCREENSHOT="${PREFIX}.dosnav-palette.ppm"
DOSNAV_FINAL_SCREENSHOT="${PREFIX}.dosnav-final.ppm"
MON_SOCK="/tmp/ciukios-full-dos-compat-smoke.monitor.sock"
DOSNAV_STABLE_SEC="${DOSNAV_STABLE_SEC:-10}"
DOSNAV_MOUSE_SETTLE_SEC="${DOSNAV_MOUSE_SETTLE_SEC:-3}"
DOSNAV_COLOR_SETTLE_SEC="${DOSNAV_COLOR_SETTLE_SEC:-5}"
QEMU_TEST_ACCEL="${QEMU_TEST_ACCEL:-kvm}"
QEMU_TEST_MEMORY_MB="${QEMU_TEST_MEMORY_MB:-256}"

ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""

usage() {
  cat << 'TXT'
Usage: scripts/qemu_test_full_dos_compat_smoke.sh [--no-build]

Boots the full profile headlessly and validates DOS compatibility smoke flow:
  EDIT MATRIX.TXT (shell alias for \APPS\CIUKEDIT.COM)
  wait for [CIUKEDIT:BOOT] and [CIUKEDIT:READY]
  submit one input line, save with F2, and exit with F10
  wait for [CIUKEDIT:SAVED] and [CIUKEDIT:OK]
  verify prompt returns
  run CIUKRTST.COM as an external runtime/DOS API probe
  wait for [CIUKRTST] ... STATE=PASS
  verify prompt returns
  run MOUSECB.COM and wait for its INT 33h callback to be armed
  inject real PS/2 movement, left-press, and left-release events
  require callback button state and one-shot press/release counters to pass
  verify prompt returns
  run the external GFXSTAR.COM command
  wait for [GFXSTAR] PASS
  verify prompt returns
  if third_party/DOSNavigator/DN.COM is present:
    cd \APPS\DOSNAV
    verify prompt changes to C:\APPS\DOSNAV>
    run DN.COM
    wait for a DOSNavigator startup banner marker
    inject real PS/2 movement and click events and require it to remain active
    exercise Options -> Colors -> VGA palette and confirm both dialogs
    require it to remain alive, then use DOSNavigator's native Alt+X Quit
    command and verify that the shell prompt returns
  otherwise: print skip/pass note and keep the lane green
  return to \APPS and rerun CIUKRTST, the INT 33h callback probe, and
  GFXSTAR in the same boot to catch leaked process, mouse, or video state

Artifacts:
  build/full/qemu-full-dos-compat-smoke.{serial.log,strings.log,stderr.log,commands.log,meta}

Environment:
  QEMU_TEST_ACCEL     Test accelerator: kvm or tcg (default: kvm).
  QEMU_TEST_MEMORY_MB Test RAM in MiB (default: 256).
  IMG                 Boot image override (default: build/full/ciukios-full.img).
TXT
}

mark_fail() {
  local marker="$1"
  local detail="$2"
  echo "[dos-compat-smoke] FAIL ${marker}: ${detail}" >&2
  if [[ -f "$STDERR_LOG" ]]; then
    tail -n 40 "$STDERR_LOG" >&2 || true
  fi
  if [[ -f "$SERIAL_LOG" ]]; then
    tail -n 200 "$SERIAL_LOG" >&2 || true
  fi
  exit 1
}

mark_pass() {
  local marker="$1"
  echo "[dos-compat-smoke] PASS ${marker}"
}

cleanup_active_qemu() {
  if [[ "$ACTIVE_QEMU_PID" -ne 0 ]] && kill -0 "$ACTIVE_QEMU_PID" >/dev/null 2>&1; then
    if [[ -n "$ACTIVE_MON_SOCK" ]] && [[ -S "$ACTIVE_MON_SOCK" ]] && [[ -n "$ACTIVE_CMD_LOG" ]]; then
      hmp "$ACTIVE_MON_SOCK" "$ACTIVE_CMD_LOG" "quit" >/dev/null 2>&1 || true
    fi
    kill "$ACTIVE_QEMU_PID" >/dev/null 2>&1 || true
    wait "$ACTIVE_QEMU_PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "$ACTIVE_MON_SOCK" ]]; then
    rm -f "$ACTIVE_MON_SOCK"
  fi
}

trap cleanup_active_qemu EXIT

need_cmd() {
  local c="$1"
  if ! command -v "$c" >/dev/null 2>&1; then
    mark_fail "CMD_${c}" "missing command: $c"
  fi
}

pick_qemu() {
  if [[ -n "${QEMU_BIN:-}" ]]; then
    echo "$QEMU_BIN"
    return 0
  fi
  if command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "qemu-system-i386"
    return 0
  fi
  if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64"
    return 0
  fi
  return 1
}

file_size() {
  local file="$1"
  if [[ -f "$file" ]]; then
    wc -c < "$file"
  else
    echo 0
  fi
}

wait_for_socket() {
  local sock="$1"
  local timeout_sec="$2"
  local start now
  start="$(date +%s)"
  while true; do
    if [[ -S "$sock" ]]; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
    sleep 0.05
  done
}

wait_for_regex_from_offset() {
  local file="$1"
  local pattern="$2"
  local offset="$3"
  local timeout_sec="$4"
  local start now
  start="$(date +%s)"
  while true; do
    if [[ -f "$file" ]] && "$SERIAL_NORMALIZER" --offset "$offset" "$file" | grep -Eiq "$pattern"; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start >= timeout_sec )); then
      return 1
    fi
    sleep 0.05
  done
}

hmp() {
  local sock="$1"
  local cmd_log="$2"
  local cmd="$3"
  local out rc
  echo "[HMP] $cmd" >> "$cmd_log"
  set +e
  out="$(printf '%s\n' "$cmd" | socat - UNIX-CONNECT:"$sock" 2>&1)"
  rc=$?
  set -e
  if [[ -n "$out" ]]; then
    printf '%s\n' "$out" >> "$cmd_log"
  fi
  echo "[HMP_RC] $cmd => $rc" >> "$cmd_log"
  return $rc
}

send_key() {
  local sock="$1"
  local cmd_log="$2"
  local key="$3"
  hmp "$sock" "$cmd_log" "sendkey $key" >/dev/null 2>&1 || return 1
  return 0
}

send_dosnav_key() {
  local sock="$1"
  local cmd_log="$2"
  local key="$3"
  # DOSNavigator owns IRQ1 while active.  Keep the make code asserted long
  # enough to survive an HMP command landing between two of its polling ticks,
  # then leave a full UI tick before the next raw keyboard event.
  hmp "$sock" "$cmd_log" "sendkey $key 50" >/dev/null 2>&1 || return 1
  sleep 0.5
  return 0
}

send_dosnav_chord() {
  local sock="$1"
  local cmd_log="$2"
  local key="$3"
  hmp "$sock" "$cmd_log" "sendkey $key" >/dev/null 2>&1 || return 1
  sleep 0.5
  return 0
}

send_text() {
  local sock="$1"
  local cmd_log="$2"
  local txt="$3"
  local i ch key

  for ((i=0; i<${#txt}; i++)); do
    ch="${txt:i:1}"
    case "$ch" in
      ' ') key="spc" ;;
      '.') key="dot" ;;
      '/') key="slash" ;;
      '\') key="backslash" ;;
      '-') key="minus" ;;
      ':') key="shift-semicolon" ;;
      [A-Z]) key="shift-$(printf '%s' "$ch" | tr 'A-Z' 'a-z')" ;;
      [a-z0-9]) key="$ch" ;;
      *) continue ;;
    esac
    send_key "$sock" "$cmd_log" "$key" || return 1
  done

  return 0
}

send_text_and_enter() {
  local sock="$1"
  local cmd_log="$2"
  local txt="$3"

  send_text "$sock" "$cmd_log" "$txt" || return 1
  send_key "$sock" "$cmd_log" ret || return 1
  return 0
}

validate_ciukedit_screens() {
  python3 - "$CIUKEDIT_MAIN_SCREENSHOT" "$CIUKEDIT_HELP_SCREENSHOT" "$CIUKEDIT_WARNING_SCREENSHOT" <<'PY'
import pathlib
import sys

def ppm(path_text):
    path = pathlib.Path(path_text)
    with path.open("rb") as fh:
        if fh.readline().strip() != b"P6":
            raise SystemExit(f"{path}: not a binary PPM screenshot")
        line = fh.readline()
        while line.startswith(b"#"):
            line = fh.readline()
        width, height = map(int, line.split())
        maximum = int(fh.readline())
        pixels = fh.read()
    if width < 640 or height < 400 or maximum != 255:
        raise SystemExit(f"{path}: unexpected geometry/range {width}x{height}/{maximum}")
    colors = {pixels[i:i + 3] for i in range(0, len(pixels), 3)}
    if len(colors) < 4:
        raise SystemExit(f"{path}: editor palette collapsed ({len(colors)} colors)")
    return width, height, pixels

main = ppm(sys.argv[1])
help_screen = ppm(sys.argv[2])
warning = ppm(sys.argv[3])
if main[:2] != help_screen[:2] or main[:2] != warning[:2]:
    raise SystemExit("CIUKEDIT screenshots changed geometry")
if main[2] == help_screen[2]:
    raise SystemExit("F1 help overlay did not change the editor frame")
if main[2] == warning[2]:
    raise SystemExit("dirty-exit warning did not change the editor frame")
PY
}

validate_dosnav_dialog_screens() {
  python3 - "$DOSNAV_BASELINE_SCREENSHOT" "$DOSNAV_COLORS_SCREENSHOT" \
    "$DOSNAV_PALETTE_SCREENSHOT" "$DOSNAV_FINAL_SCREENSHOT" <<'PY'
import sys


def read_ppm(path):
    raw = open(path, "rb").read()
    pos = 0

    def token():
        nonlocal pos
        while pos < len(raw):
            if raw[pos:pos + 1] == b"#":
                pos = raw.find(b"\n", pos)
                if pos < 0:
                    raise ValueError(f"unterminated PPM comment in {path}")
            elif raw[pos] in b" \t\r\n":
                pos += 1
            else:
                break
        end = pos
        while end < len(raw) and raw[end] not in b" \t\r\n":
            end += 1
        value = raw[pos:end]
        pos = end
        return value

    if token() != b"P6":
        raise ValueError(f"{path} is not a binary PPM")
    width = int(token())
    height = int(token())
    if int(token()) != 255:
        raise ValueError(f"{path} has an unsupported PPM max value")
    if raw[pos:pos + 2] == b"\r\n":
        pos += 2
    elif pos < len(raw) and raw[pos] in b" \t\r\n":
        pos += 1
    else:
        raise ValueError(f"{path} has no separator before its pixel payload")
    pixels = raw[pos:pos + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path} has a truncated pixel payload")
    return width, height, pixels


def central_pixels(frame):
    width, height, pixels = frame
    x0, x1 = width * 30 // 100, width * 72 // 100
    y0, y1 = height * 5 // 100, height * 78 // 100
    for y in range(y0, y1):
        start = (y * width + x0) * 3
        end = (y * width + x1) * 3
        row = pixels[start:end]
        for i in range(0, len(row), 3):
            yield row[i:i + 3]


def changed(a, b):
    if a[:2] != b[:2]:
        raise ValueError("DOSNavigator screenshots have different dimensions")
    return sum(pa != pb for pa, pb in zip(central_pixels(a), central_pixels(b)))


baseline, colors, palette, final = map(read_ppm, sys.argv[1:])
area = sum(1 for _ in central_pixels(colors))
colors_changed = changed(baseline, colors)
palette_changed = changed(colors, palette)
final_changed = changed(colors, final)
cyan = sum(
    px[0] < 80 and px[1] > 100 and px[2] > 100
    for px in central_pixels(colors)
)
width, height, pixels = colors
swatches = 0
for y in range(height * 10 // 100, height * 65 // 100):
    for x in range(width * 70 // 100, width * 90 // 100):
        i = (y * width + x) * 3
        r, g, b = pixels[i:i + 3]
        if (r > 120 and g < 100 and b < 100) or (r > 160 and g > 160 and b < 120):
            swatches += 1

if colors_changed < area // 8:
    raise SystemExit(f"Colors dialog is not visible (central changes={colors_changed})")
if cyan < area // 12:
    raise SystemExit(f"Colors item/palette area is not visible (cyan pixels={cyan})")
if swatches < 500:
    raise SystemExit(f"Colors foreground/background swatches are not visible (pixels={swatches})")
if palette_changed < area // 12:
    raise SystemExit(f"VGA palette dialog is not visible (central changes={palette_changed})")
if final_changed < area // 8:
    raise SystemExit(f"Colors dialog did not close cleanly (central changes={final_changed})")

print(
    f"colors_changed={colors_changed} cyan={cyan} swatches={swatches} "
    f"palette_changed={palette_changed} final_changed={final_changed}"
)
PY
}

validate_dosnav_single_arrow_step() {
  python3 - "$DOSNAV_NAV_HOME_SCREENSHOT" "$DOSNAV_NAV_DOWN_SCREENSHOT" <<'PY'
import sys


def read_ppm(path):
    raw = open(path, "rb").read()
    fields = raw.split(None, 4)
    if len(fields) != 5 or fields[0] != b"P6":
        raise ValueError(f"{path}: invalid binary PPM")
    width, height, maximum = map(int, fields[1:4])
    if maximum != 255 or len(fields[4]) != width * height * 3:
        raise ValueError(f"{path}: invalid PPM dimensions/data")
    return width, height, fields[4]


def selection_center(path):
    width, height, pixels = read_ppm(path)
    if width < 715 or height < 295:
        raise ValueError(f"{path}: unexpected screenshot size {width}x{height}")
    selected_lines = []
    for y in range(32, 295):
        cyan = 0
        for x in range(3, 715):
            offset = (y * width + x) * 3
            red, green, blue = pixels[offset:offset + 3]
            if red < 40 and green > 120 and blue > 120:
                cyan += 1
        if cyan > 50:
            selected_lines.append(y)
    if not 12 <= len(selected_lines) <= 20:
        raise ValueError(
            f"{path}: cannot isolate one DOSNavigator selection row "
            f"(lines={len(selected_lines)})"
        )
    return sum(selected_lines) / len(selected_lines)


before = selection_center(sys.argv[1])
after = selection_center(sys.argv[2])
step = after - before
if not 14.0 <= step <= 18.0:
    raise SystemExit(
        f"dedicated Down moved {step:.1f} pixels instead of one 16-pixel row "
        f"(before={before:.1f}, after={after:.1f})"
    )
print(f"dedicated Down single-row delta={step:.1f}px")
PY
}

wait_for_prompt_from_offset() {
  local offset="$1"
  local timeout_sec="$2"
  local marker="$3"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$APPS_PROMPT_PATTERN" "$offset" "$timeout_sec"; then
    mark_fail "$marker" "C:\\APPS prompt did not appear"
  fi
  mark_pass "$marker"
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
      echo "[dos-compat-smoke] ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd socat
need_cmd strings
need_cmd timeout
need_cmd python3
need_cmd rg
need_cmd mtype
need_cmd sha256sum

if (( DO_BUILD )); then
  echo "[dos-compat-smoke] build step"
  bash scripts/build_full.sh
fi

if [[ ! -f "$IMG" ]]; then
  mark_fail "IMAGE" "missing image: $IMG"
fi

if rg -qi 'env_doom_exe_path|path_gem_exe_abs|int21_find_try_gem_special|int21_patch_gem_desktop_tree|PCDOOM|DOOMVAN|DOSNAV|DN\.COM' src/boot/floppy_stage1.asm; then
  mark_fail "GENERIC_EXEC_MEMORY_POLICY" "DOS runtime contains a title-specific executable rule"
fi
mark_pass "GENERIC_EXEC_MEMORY_POLICY"

if [[ -f third_party/DOSNavigator/DN.COM ]]; then
  DOSNAV_SOURCE_HASH="$(sha256sum third_party/DOSNavigator/DN.COM | awk '{print $1}')"
  DOSNAV_IMAGE_HASH="$(mtype -i "$IMG" ::APPS/DOSNAV/DN.COM | sha256sum | awk '{print $1}')"
  if [[ "$DOSNAV_IMAGE_HASH" != "$DOSNAV_SOURCE_HASH" ]]; then
    mark_fail "DOSNAV_UPSTREAM_BINARY" "image DN.COM differs from the unmodified bundled payload"
  fi
  mark_pass "DOSNAV_UPSTREAM_BINARY"
fi

QEMU_CMD="$(pick_qemu || true)"
if [[ -z "$QEMU_CMD" ]]; then
  mark_fail "QEMU" "qemu-system-i386/x86_64 not found"
fi
case "$QEMU_TEST_ACCEL" in
  kvm|tcg) ;;
  *) mark_fail "QEMU_ACCEL" "QEMU_TEST_ACCEL must be kvm or tcg" ;;
esac
if [[ ! "$QEMU_TEST_MEMORY_MB" =~ ^[0-9]+$ ]] \
  || (( QEMU_TEST_MEMORY_MB < 64 || QEMU_TEST_MEMORY_MB > 4096 )); then
  mark_fail "QEMU_MEMORY" "QEMU_TEST_MEMORY_MB must be an integer from 64 to 4096"
fi

mkdir -p build/full
rm -f "$SERIAL_LOG" "$STRINGS_LOG" "$STDERR_LOG" "$CMD_LOG" "$META_LOG" "$MON_SOCK" \
  "$CIUKEDIT_MAIN_SCREENSHOT" "$CIUKEDIT_HELP_SCREENSHOT" "$CIUKEDIT_WARNING_SCREENSHOT"

QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-300}"
PROMPT_TIMEOUT_SEC="${DOS_COMPAT_PROMPT_TIMEOUT_SEC:-120}"
APP_TIMEOUT_SEC="${DOS_COMPAT_APP_TIMEOUT_SEC:-120}"
EDITOR_INPUT_LINE="${DOS_COMPAT_EDITOR_INPUT_LINE:-compat smoke}"
CIUKEDIT_COMMAND='edit MATRIX.TXT'
GFXSTAR_COMMAND='gfxstar'
CIUKRTST_COMMAND='ciukrtst'
MOUSECB_COMMAND='mousecb'
DOSNAV_PAYLOAD='third_party/DOSNavigator/DN.COM'
DOSNAV_DIR_COMMAND='cd \APPS\DOSNAV'
DOSNAV_COMMAND="${DOSNAV_COMMAND:-run DN.COM}"
DOSNAV_PRESENT=0
if [[ -f "$DOSNAV_PAYLOAD" ]]; then
  DOSNAV_PRESENT=1
fi

PROMPT_PREFIX='C{1,2}i{1,2}u{1,2}k{1,2}i{1,2}O{1,2}S{1,2}[[:space:]]+(S{1,2}H{1,2}E{1,2}L{2,4}[[:space:]]+)?'
BS='[\\]'
ROOT_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}>{1,2}"
APPS_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{0,2}>{1,2}"
SHELL_PROMPT_PATTERN="(${ROOT_PROMPT_PATTERN})|(${APPS_PROMPT_PATTERN})"
DOSNAV_PROMPT_PATTERN="${PROMPT_PREFIX}C{1,2}:{1,2}${BS}{1,2}A{1,2}P{2,4}S{1,2}${BS}{1,2}D{1,2}O{1,2}S{1,2}N{1,2}A{1,2}V{1,2}${BS}{0,2}>{1,2}"
CIUKEDIT_BOOT_PATTERN='\[CIUKEDIT:BOOT\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}B{1,2}O{2,4}T{1,2}\]{1,2}'
CIUKEDIT_READY_PATTERN='\[CIUKEDIT:READY\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}R{1,2}E{1,2}A{1,2}D{1,2}Y{1,2}\]{1,2}'
CIUKEDIT_SAVED_PATTERN='\[CIUKEDIT:SAVED\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}S{1,2}A{1,2}V{1,2}E{1,2}D{1,2}\]{1,2}'
CIUKEDIT_OPEN_PATTERN='\[CIUKEDIT:OPEN\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}O{1,2}P{1,2}E{1,2}N{1,2}\]{1,2}'
CIUKEDIT_OK_PATTERN='\[CIUKEDIT:OK\]|\[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}O{1,2}K{1,2}\]{1,2}'
CIUKEDIT_CONTENT_PATTERN='s{1,2}e{1,2}c{1,2}o{1,2}n{1,2}d{1,2}[[:space:]]+l{1,2}i{1,2}n{1,2}e{1,2}'
CIUKRTST_PASS_PATTERN='\[CIUKRTST\][[:space:]]+OWNER=CIUKIDOS[[:space:]]+ABI=2[[:space:]]+SERVICES=11[[:space:]]+CHAIN=0[[:space:]]+STATE=PASS'
MOUSECB_READY_PATTERN='\[MOUSECB\][[:space:]]+READY'
MOUSECB_PASS_PATTERN='\[MOUSECB\][[:space:]]+PASS'
GFXSTAR_PASS_PATTERN='\[GFXSTAR\][[:space:]]+PASS|\[{1,2}G{1,2}F{1,2}X{1,2}S{1,2}T{1,2}A{1,2}R{1,2}\]{1,2}[[:space:]]+P{1,2}A{1,2}S{2,4}'
DOSNAV_START_PATTERN='Dos[[:space:]]+Navigator|D{1,2}o{1,2}s{1,2}[[:space:]]+N{1,2}a{1,2}v{1,2}i{1,2}g{1,2}a{1,2}t{1,2}o{1,2}r|RIT[[:space:]]+Research[[:space:]]+Labs|R{1,2}I{1,2}T{1,2}[[:space:]]+R{1,2}e{1,2}s{1,2}e{1,2}a{1,2}r{1,2}c{1,2}h{1,2}[[:space:]]+L{1,2}a{1,2}b{1,2}s{1,2}'

QEMU_ARGS=(
  -accel "$QEMU_TEST_ACCEL"
  -machine pc,vmport=off,i8042=on
  -cpu pentium3
  -m "$QEMU_TEST_MEMORY_MB"
  -drive "file=$IMG,format=raw,if=ide"
  -snapshot
  -boot c
  -nographic
  -chardev "file,id=ser0,path=$SERIAL_LOG"
  -serial chardev:ser0
  -monitor "unix:$MON_SOCK,server,nowait"
  -no-reboot
  -no-shutdown
)

set +e
timeout "$QEMU_TIMEOUT_SEC" "$QEMU_CMD" "${QEMU_ARGS[@]}" >/dev/null 2>"$STDERR_LOG" &
QEMU_PID=$!
set -e

ACTIVE_QEMU_PID=$QEMU_PID
ACTIVE_MON_SOCK="$MON_SOCK"
ACTIVE_CMD_LOG="$CMD_LOG"

if ! wait_for_socket "$MON_SOCK" 20; then
  mark_fail "MONITOR_SOCKET_READY" "monitor socket not ready"
fi
mark_pass "MONITOR_SOCKET_READY"

if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
  mark_fail "QEMU_EARLY_EXIT" "qemu exited before shell prompt"
fi

wait_for_prompt_from_offset 0 "$PROMPT_TIMEOUT_SEC" "INITIAL_PROMPT_DETECTED"

CIUKEDIT_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$CIUKEDIT_COMMAND" || mark_fail "SEND_CIUKEDIT_COMMAND" "cannot send CIUKEDIT command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_BOOT_PATTERN" "$CIUKEDIT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_BOOT" "missing [CIUKEDIT:BOOT] marker"
fi
mark_pass "CIUKEDIT_BOOT"

if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_READY_PATTERN" "$CIUKEDIT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_READY" "missing [CIUKEDIT:READY] marker"
fi
mark_pass "CIUKEDIT_READY"

CIUKEDIT_INPUT_OFFSET="$(file_size "$SERIAL_LOG")"
send_text "$MON_SOCK" "$CMD_LOG" "$EDITOR_INPUT_LINE" || mark_fail "SEND_CIUKEDIT_INPUT" "cannot send CIUKEDIT input line"
send_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "SEND_CIUKEDIT_INPUT" "cannot insert a CIUKEDIT newline"
send_text "$MON_SOCK" "$CMD_LOG" 'second line' || mark_fail "SEND_CIUKEDIT_INPUT" "cannot send the second CIUKEDIT line"
send_key "$MON_SOCK" "$CMD_LOG" up || mark_fail "SEND_CIUKEDIT_NAVIGATION" "cannot move CIUKEDIT up"
send_key "$MON_SOCK" "$CMD_LOG" end || mark_fail "SEND_CIUKEDIT_NAVIGATION" "cannot move CIUKEDIT to line end"
send_text "$MON_SOCK" "$CMD_LOG" ' updated' || mark_fail "SEND_CIUKEDIT_INPUT" "cannot update the first CIUKEDIT line"
sleep 5
hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$CIUKEDIT_MAIN_SCREENSHOT\"" >/dev/null 2>&1 \
  || mark_fail "CIUKEDIT_MAIN_VISUAL" "cannot capture CIUKEDIT main screen"
send_key "$MON_SOCK" "$CMD_LOG" f1 || mark_fail "SEND_CIUKEDIT_HELP" "cannot open CIUKEDIT help"
sleep 0.5
hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$CIUKEDIT_HELP_SCREENSHOT\"" >/dev/null 2>&1 \
  || mark_fail "CIUKEDIT_HELP_VISUAL" "cannot capture CIUKEDIT help screen"
send_key "$MON_SOCK" "$CMD_LOG" esc || mark_fail "SEND_CIUKEDIT_HELP_CLOSE" "cannot close CIUKEDIT help"
sleep 0.5
send_key "$MON_SOCK" "$CMD_LOG" f10 || mark_fail "SEND_CIUKEDIT_DIRTY_EXIT" "cannot request CIUKEDIT dirty exit"
sleep 2
hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$CIUKEDIT_WARNING_SCREENSHOT\"" >/dev/null 2>&1 \
  || mark_fail "CIUKEDIT_WARNING_VISUAL" "cannot capture CIUKEDIT dirty-exit warning"
validate_ciukedit_screens || mark_fail "CIUKEDIT_VISUAL" "CIUKEDIT visual state validation failed"
mark_pass "CIUKEDIT_VISUAL"
send_key "$MON_SOCK" "$CMD_LOG" f2 || mark_fail "SEND_CIUKEDIT_SAVE" "cannot send CIUKEDIT F2 save key"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_SAVED_PATTERN" "$CIUKEDIT_INPUT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_SAVED" "missing [CIUKEDIT:SAVED] marker"
fi
mark_pass "CIUKEDIT_SAVED"
send_key "$MON_SOCK" "$CMD_LOG" f10 || mark_fail "SEND_CIUKEDIT_EXIT" "cannot send CIUKEDIT F10 exit key"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_OK_PATTERN" "$CIUKEDIT_INPUT_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_OK" "missing [CIUKEDIT:OK] marker"
fi
mark_pass "CIUKEDIT_OK"

wait_for_prompt_from_offset "$CIUKEDIT_INPUT_OFFSET" "$APP_TIMEOUT_SEC" "CIUKEDIT_PROMPT_RETURNED"

CIUKEDIT_TYPE_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'type MATRIX.TXT' || mark_fail "SEND_CIUKEDIT_TYPE" "cannot TYPE the saved CIUKEDIT document"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_CONTENT_PATTERN" "$CIUKEDIT_TYPE_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_CONTENT" "saved multiline content was not readable through DOS"
fi
mark_pass "CIUKEDIT_CONTENT"
wait_for_prompt_from_offset "$CIUKEDIT_TYPE_OFFSET" "$APP_TIMEOUT_SEC" "CIUKEDIT_TYPE_PROMPT_RETURNED"

CIUKEDIT_REOPEN_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$CIUKEDIT_COMMAND" || mark_fail "SEND_CIUKEDIT_REOPEN" "cannot reopen the CIUKEDIT document"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKEDIT_OPEN_PATTERN" "$CIUKEDIT_REOPEN_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKEDIT_REOPEN" "CIUKEDIT did not reopen the saved file"
fi
mark_pass "CIUKEDIT_REOPEN"
send_key "$MON_SOCK" "$CMD_LOG" f10 || mark_fail "SEND_CIUKEDIT_REOPEN_EXIT" "cannot exit the reopened CIUKEDIT document"
wait_for_prompt_from_offset "$CIUKEDIT_REOPEN_OFFSET" "$APP_TIMEOUT_SEC" "CIUKEDIT_REOPEN_PROMPT_RETURNED"

CIUKRTST_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$CIUKRTST_COMMAND" || mark_fail "SEND_CIUKRTST_COMMAND" "cannot send CIUKRTST command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKRTST_PASS_PATTERN" "$CIUKRTST_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKRTST_PASS" "missing CIUKRTST runtime/DOS ownership marker"
fi
mark_pass "CIUKRTST_PASS"

if ! wait_for_regex_from_offset "$SERIAL_LOG" "$SHELL_PROMPT_PATTERN" "$CIUKRTST_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "CIUKRTST_PROMPT_RETURNED" "shell prompt did not appear"
fi
mark_pass "CIUKRTST_PROMPT_RETURNED"

MOUSECB_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$MOUSECB_COMMAND" || mark_fail "SEND_MOUSECB_COMMAND" "cannot send MOUSECB command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$MOUSECB_READY_PATTERN" "$MOUSECB_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "MOUSECB_READY" "INT 33h callback probe did not arm"
fi
mark_pass "MOUSECB_READY"
hmp "$MON_SOCK" "$CMD_LOG" "mouse_move 80 40 0" >/dev/null 2>&1 \
  || mark_fail "MOUSECB_EVENT" "cannot inject PS/2 mouse movement"
hmp "$MON_SOCK" "$CMD_LOG" "mouse_button 1" >/dev/null 2>&1 \
  || mark_fail "MOUSECB_EVENT" "cannot inject the left-button press"
sleep 0.2
hmp "$MON_SOCK" "$CMD_LOG" "mouse_button 0" >/dev/null 2>&1 \
  || mark_fail "MOUSECB_EVENT" "cannot inject the left-button release"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$MOUSECB_PASS_PATTERN" "$MOUSECB_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "MOUSECB_CALLBACK_PASS" "callback did not return cleanly to the DOS application"
fi
mark_pass "MOUSECB_CALLBACK_PASS"
mark_pass "MOUSECB_NESTED_IRQ_DRAIN"
wait_for_prompt_from_offset "$MOUSECB_OFFSET" "$APP_TIMEOUT_SEC" "MOUSECB_PROMPT_RETURNED"

APPS_SYNC_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'cd \APPS' || mark_fail "SEND_APPS_SYNC_CD_COMMAND" "cannot send APPS sync directory change command"
wait_for_prompt_from_offset "$APPS_SYNC_OFFSET" "$APP_TIMEOUT_SEC" "APPS_SYNC_PROMPT_RETURNED"

GFXSTAR_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$GFXSTAR_COMMAND" || mark_fail "SEND_GFXSTAR_COMMAND" "cannot send GFXSTAR command"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$GFXSTAR_PASS_PATTERN" "$GFXSTAR_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "GFXSTAR_PASS" "missing [GFXSTAR] PASS marker"
fi
mark_pass "GFXSTAR_PASS"

wait_for_prompt_from_offset "$GFXSTAR_OFFSET" "$APP_TIMEOUT_SEC" "GFXSTAR_PROMPT_RETURNED"

if (( DOSNAV_PRESENT )); then
  DOSNAV_CD_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$DOSNAV_DIR_COMMAND" || mark_fail "SEND_DOSNAV_CD_COMMAND" "cannot send DOSNavigator directory change command"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DOSNAV_PROMPT_PATTERN" "$DOSNAV_CD_OFFSET" "$APP_TIMEOUT_SEC"; then
    mark_fail "DOSNAV_PROMPT_CHANGED" "C:\APPS\DOSNAV prompt did not appear"
  fi
  mark_pass "DOSNAV_PROMPT_CHANGED"

  DOSNAV_RUN_OFFSET="$(file_size "$SERIAL_LOG")"
  send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$DOSNAV_COMMAND" || mark_fail "SEND_DOSNAV_COMMAND" "cannot send DOSNavigator launch command"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DOSNAV_START_PATTERN" "$DOSNAV_RUN_OFFSET" "$APP_TIMEOUT_SEC"; then
    mark_fail "DOSNAV_START" "missing DOSNavigator startup banner marker"
  fi
  mark_pass "DOSNAV_START"
  sleep "$DOSNAV_STABLE_SEC"
  if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
    mark_fail "DOSNAV_MEMORY_STABLE" "QEMU exited while DOSNavigator was active"
  fi
  if "$SERIAL_NORMALIZER" --offset "$DOSNAV_RUN_OFFSET" "$SERIAL_LOG" \
      | grep -Eiq 'not enough memory|insufficient memory|out of memory|memory allocation|EXEC FAIL|runtime error'; then
    mark_fail "DOSNAV_MEMORY_STABLE" "DOSNavigator reported a memory/runtime failure"
  fi
  mark_pass "DOSNAV_MEMORY_STABLE"

  DOSNAV_MOUSE_OFFSET="$(file_size "$SERIAL_LOG")"
  hmp "$MON_SOCK" "$CMD_LOG" "mouse_move 80 40 0" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_MOUSE_KEYS" "cannot inject PS/2 mouse movement"
  hmp "$MON_SOCK" "$CMD_LOG" "mouse_button 1" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_MOUSE_KEYS" "cannot inject the left-button press"
  sleep 0.2
  hmp "$MON_SOCK" "$CMD_LOG" "mouse_button 0" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_MOUSE_KEYS" "cannot inject the left-button release"
  sleep "$DOSNAV_MOUSE_SETTLE_SEC"
  if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
    mark_fail "DOSNAV_MOUSE_STABLE" "QEMU exited after a DOSNavigator mouse event"
  fi
  if "$SERIAL_NORMALIZER" --offset "$DOSNAV_MOUSE_OFFSET" "$SERIAL_LOG" \
      | grep -Eiq "$DOSNAV_PROMPT_PATTERN"; then
    mark_fail "DOSNAV_MOUSE_STABLE" "DOSNavigator returned to the shell after mouse movement/click"
  fi
  mark_pass "DOSNAV_MOUSE_STABLE"

  # Reproduce the reported failure exactly: start on ".." and press the
  # dedicated Down key once.  HMP's `down` emits the E0-prefixed sequence,
  # whereas kp_2 would hide the compatibility bug.
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" kp_7 \
    || mark_fail "DOSNAV_ARROW_KEYS" "cannot select the first panel row"
  hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$DOSNAV_NAV_HOME_SCREENSHOT\"" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_ARROW_SCREEN" "cannot capture the initial navigation row"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" down \
    || mark_fail "DOSNAV_ARROW_KEYS" "cannot inject the dedicated Down key"
  hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$DOSNAV_NAV_DOWN_SCREENSHOT\"" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_ARROW_SCREEN" "cannot capture the row after dedicated Down"
  if ! DOSNAV_ARROW_DETAIL="$(validate_dosnav_single_arrow_step 2>&1)"; then
    mark_fail "DOSNAV_ARROW_SINGLE_STEP" "$DOSNAV_ARROW_DETAIL"
  fi
  mark_pass "DOSNAV_ARROW_SINGLE_STEP"

  hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$DOSNAV_BASELINE_SCREENSHOT\"" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_BASELINE_SCREEN" "cannot capture DOSNavigator before Colors"

  # Alt+O opens the intended top-level menu deterministically.  F10 followed
  # by a plain O can instead activate "Output window" in the File menu.  HMP
  # bypasses the visual runner's legacy-navigation keymap, so drive the raw
  # handler with equivalent keypad navigation codes.  End selects Load
  # palette; two Up events select Colors.
  send_dosnav_chord "$MON_SOCK" "$CMD_LOG" alt-o || mark_fail "DOSNAV_COLORS_KEYS" "cannot open Options"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" kp_1 || mark_fail "DOSNAV_COLORS_KEYS" "cannot select the last Options item"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" kp_8 || mark_fail "DOSNAV_COLORS_KEYS" "cannot select Store palette"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" kp_8 || mark_fail "DOSNAV_COLORS_KEYS" "cannot select Colors"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "DOSNAV_COLORS_KEYS" "cannot open Colors"
  sleep 1
  hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$DOSNAV_COLORS_SCREENSHOT\"" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_COLORS_SCREEN" "cannot capture the Colors dialog"
  send_dosnav_chord "$MON_SOCK" "$CMD_LOG" alt-v || mark_fail "DOSNAV_COLORS_KEYS" "cannot open VGA palette"
  sleep 1
  hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$DOSNAV_PALETTE_SCREENSHOT\"" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_PALETTE_SCREEN" "cannot capture the VGA palette dialog"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "DOSNAV_COLORS_KEYS" "cannot confirm VGA palette"
  DOSNAV_COLOR_COMMIT_OFFSET="$(file_size "$SERIAL_LOG")"
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" ret || mark_fail "DOSNAV_COLORS_KEYS" "cannot confirm Colors"
  sleep "$DOSNAV_COLOR_SETTLE_SEC"
  hmp "$MON_SOCK" "$CMD_LOG" "screendump \"$ROOT_DIR/$DOSNAV_FINAL_SCREENSHOT\"" >/dev/null 2>&1 \
    || mark_fail "DOSNAV_FINAL_SCREEN" "cannot capture DOSNavigator after Colors"
  if ! DOSNAV_VISUAL_DETAIL="$(validate_dosnav_dialog_screens 2>&1)"; then
    mark_fail "DOSNAV_COLORS_VISUAL" "$DOSNAV_VISUAL_DETAIL"
  fi
  mark_pass "DOSNAV_COLORS_VISUAL"

  if ! kill -0 "$QEMU_PID" >/dev/null 2>&1; then
    mark_fail "DOSNAV_XMS_COLORS_STABLE" "QEMU exited after the Colors/XMS overlay flow"
  fi
  if "$SERIAL_NORMALIZER" --offset "$DOSNAV_COLOR_COMMIT_OFFSET" "$SERIAL_LOG" \
      | grep -Eiq "$DOSNAV_PROMPT_PATTERN"; then
    mark_fail "DOSNAV_XMS_COLORS_STABLE" "DOSNavigator returned to the shell after confirming Colors"
  fi
  mark_pass "DOSNAV_XMS_COLORS_STABLE"

  DOSNAV_EXIT_OFFSET="$(file_size "$SERIAL_LOG")"
  # Alt+X is DOSNavigator's own Quit action.  Typing EXIT opens its child
  # command processor and therefore tests COMMAND.COM, not application exit.
  send_dosnav_chord "$MON_SOCK" "$CMD_LOG" alt-x \
    || mark_fail "DOSNAV_EXIT_KEYS" "cannot invoke DOSNavigator Alt+X Quit"
  # Confirm the standard "Do you wish to quit" dialog.  If a saved profile
  # disables confirmation, Enter is harmless at the restored shell prompt.
  send_dosnav_key "$MON_SOCK" "$CMD_LOG" ret \
    || mark_fail "DOSNAV_EXIT_KEYS" "cannot confirm DOSNavigator Quit"
  if ! wait_for_regex_from_offset "$SERIAL_LOG" "$DOSNAV_PROMPT_PATTERN" "$DOSNAV_EXIT_OFFSET" "$APP_TIMEOUT_SEC"; then
    mark_fail "DOSNAV_EXIT_RETURN" "Alt+X Quit did not terminate DOSNavigator and restore the shell prompt"
  fi
  mark_pass "DOSNAV_EXIT_RETURN"
else
  echo "[dos-compat-smoke] PASS DOSNAV_SKIP: payload not present at $DOSNAV_PAYLOAD"
fi

# A successful application run is not enough: prove that DOSNavigator (or the
# preceding compatibility sequence when the optional payload is absent) did
# not leave PSP, INT 33h callback, keyboard, or video state behind.
POST_APPS_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" 'cd \APPS' \
  || mark_fail "POST_DOSNAV_CD" "cannot return to C:\\APPS after DOSNavigator"
wait_for_prompt_from_offset "$POST_APPS_OFFSET" "$APP_TIMEOUT_SEC" "POST_DOSNAV_APPS_PROMPT"

POST_CIUKRTST_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$CIUKRTST_COMMAND" \
  || mark_fail "POST_DOSNAV_CIUKRTST_COMMAND" "cannot rerun CIUKRTST"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$CIUKRTST_PASS_PATTERN" "$POST_CIUKRTST_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "POST_DOSNAV_CIUKRTST_PASS" "runtime/process ownership did not survive the application sequence"
fi
mark_pass "POST_DOSNAV_CIUKRTST_PASS"
wait_for_prompt_from_offset "$POST_CIUKRTST_OFFSET" "$APP_TIMEOUT_SEC" "POST_DOSNAV_CIUKRTST_PROMPT"

POST_MOUSECB_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$MOUSECB_COMMAND" \
  || mark_fail "POST_DOSNAV_MOUSECB_COMMAND" "cannot rerun the INT 33h callback probe"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$MOUSECB_READY_PATTERN" "$POST_MOUSECB_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "POST_DOSNAV_MOUSECB_READY" "post-sequence INT 33h callback probe did not arm"
fi
hmp "$MON_SOCK" "$CMD_LOG" "mouse_move -60 -30 0" >/dev/null 2>&1 \
  || mark_fail "POST_DOSNAV_MOUSECB_EVENT" "cannot inject post-sequence PS/2 movement"
hmp "$MON_SOCK" "$CMD_LOG" "mouse_button 1" >/dev/null 2>&1 \
  || mark_fail "POST_DOSNAV_MOUSECB_EVENT" "cannot inject post-sequence left-button press"
sleep 0.2
hmp "$MON_SOCK" "$CMD_LOG" "mouse_button 0" >/dev/null 2>&1 \
  || mark_fail "POST_DOSNAV_MOUSECB_EVENT" "cannot inject post-sequence left-button release"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$MOUSECB_PASS_PATTERN" "$POST_MOUSECB_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "POST_DOSNAV_MOUSECB_PASS" "post-sequence INT 33h callback did not return cleanly"
fi
mark_pass "POST_DOSNAV_MOUSECB_PASS"
wait_for_prompt_from_offset "$POST_MOUSECB_OFFSET" "$APP_TIMEOUT_SEC" "POST_DOSNAV_MOUSECB_PROMPT"

POST_GFXSTAR_OFFSET="$(file_size "$SERIAL_LOG")"
send_text_and_enter "$MON_SOCK" "$CMD_LOG" "$GFXSTAR_COMMAND" \
  || mark_fail "POST_DOSNAV_GFXSTAR_COMMAND" "cannot rerun GFXSTAR"
if ! wait_for_regex_from_offset "$SERIAL_LOG" "$GFXSTAR_PASS_PATTERN" "$POST_GFXSTAR_OFFSET" "$APP_TIMEOUT_SEC"; then
  mark_fail "POST_DOSNAV_GFXSTAR_PASS" "video mode/state did not survive the application sequence"
fi
mark_pass "POST_DOSNAV_GFXSTAR_PASS"
wait_for_prompt_from_offset "$POST_GFXSTAR_OFFSET" "$APP_TIMEOUT_SEC" "POST_DOSNAV_GFXSTAR_PROMPT"

hmp "$MON_SOCK" "$CMD_LOG" "quit" >/dev/null 2>&1 || true
set +e
wait "$QEMU_PID"
QEMU_RC=$?
set -e
if [[ "$QEMU_RC" -ne 0 ]]; then
  mark_fail "QEMU_EXIT" "unexpected qemu exit code: $QEMU_RC"
fi
ACTIVE_QEMU_PID=0
ACTIVE_MON_SOCK=""
ACTIVE_CMD_LOG=""
rm -f "$MON_SOCK"

"$SERIAL_NORMALIZER" "$SERIAL_LOG" | strings -a > "$STRINGS_LOG" || true

if ! grep -Eiq "$CIUKEDIT_BOOT_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_CIUKEDIT_BOOT" "CIUKEDIT boot marker missing in strings log"
fi
if ! grep -Eiq "$CIUKEDIT_OK_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_CIUKEDIT_OK" "CIUKEDIT ok marker missing in strings log"
fi
if ! grep -Eiq "$CIUKRTST_PASS_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_CIUKRTST_PASS" "CIUKRTST pass marker missing in strings log"
fi
if ! grep -Eiq "$GFXSTAR_PASS_PATTERN" "$STRINGS_LOG"; then
  mark_fail "STRINGS_GFXSTAR_PASS" "GFXSTAR pass marker missing in strings log"
fi
if (( DOSNAV_PRESENT )); then
  if ! grep -Eiq "$DOSNAV_START_PATTERN" "$STRINGS_LOG"; then
    mark_fail "STRINGS_DOSNAV_START" "DOSNavigator startup marker missing in strings log"
  fi
fi

{
  echo "QEMU_CMD=$QEMU_CMD"
  echo "QEMU_RC=$QEMU_RC"
  echo "IMAGE=$IMG"
  echo "SERIAL_LOG=$SERIAL_LOG"
  echo "STRINGS_LOG=$STRINGS_LOG"
  echo "STDERR_LOG=$STDERR_LOG"
  echo "COMMAND_LOG=$CMD_LOG"
  echo "PROMPT_TIMEOUT_SEC=$PROMPT_TIMEOUT_SEC"
  echo "APP_TIMEOUT_SEC=$APP_TIMEOUT_SEC"
  echo "QEMU_TIMEOUT_SEC=$QEMU_TIMEOUT_SEC"
  echo "EDITOR_INPUT_LINE=$EDITOR_INPUT_LINE"
  echo "CIUKEDIT_COMMAND=$CIUKEDIT_COMMAND"
  echo "CIUKRTST_COMMAND=$CIUKRTST_COMMAND"
  echo "MOUSECB_COMMAND=$MOUSECB_COMMAND"
  echo "GFXSTAR_COMMAND=$GFXSTAR_COMMAND"
  echo "DOSNAV_PAYLOAD=$DOSNAV_PAYLOAD"
  echo "DOSNAV_PRESENT=$DOSNAV_PRESENT"
  echo "DOSNAV_DIR_COMMAND=$DOSNAV_DIR_COMMAND"
  echo "DOSNAV_COMMAND=$DOSNAV_COMMAND"
  echo "DOSNAV_STABLE_SEC=$DOSNAV_STABLE_SEC"
  echo "DOSNAV_MOUSE_SETTLE_SEC=$DOSNAV_MOUSE_SETTLE_SEC"
  echo "DOSNAV_COLOR_SETTLE_SEC=$DOSNAV_COLOR_SETTLE_SEC"
  echo "DOS_RUNTIME_COVERAGE=CIUKRTST_owner_services_version_drive_psp_dta"
  echo "VALIDATION_FLOW=ciukedit_then_ciukrtst_then_int33_callback_probe_then_gfxstar_then_dosnavigator_mouse_and_xms_colors_if_present"
} > "$META_LOG"

if (( DOSNAV_PRESENT )); then
  echo "[dos-compat-smoke] PASS (CIUKEDIT, CIUKRTST, GFXSTAR, DOSNavigator, and same-boot post-app cleanup verified)"
else
  echo "[dos-compat-smoke] PASS (CIUKEDIT, CIUKRTST, and GFXSTAR verified; DOSNavigator skipped because payload is absent)"
fi
