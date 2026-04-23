#!/usr/bin/env bash
set -euo pipefail

CIUKIOS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

ciuk_macos_prepare_tools() {
  local prefix
  
  # Create a temporary wrapper directory for stat and other commands
  CIUKIOS_WRAPPER_BIN="${TMPDIR}ciukios-bin-$$"
  mkdir -p "$CIUKIOS_WRAPPER_BIN"
  export PATH="$CIUKIOS_WRAPPER_BIN:$PATH"
  
  # Add GNU tool paths from Homebrew (for gstat, gsed, etc)
  for prefix in /opt/homebrew /usr/local; do
    if [[ -d "$prefix/opt/coreutils/libexec/gnubin" ]]; then
      export PATH="$prefix/opt/coreutils/libexec/gnubin:$PATH"
    fi
    if [[ -d "$prefix/opt/gnu-sed/libexec/gnubin" ]]; then
      export PATH="$prefix/opt/gnu-sed/libexec/gnubin:$PATH"
    fi
    if [[ -d "$prefix/opt/findutils/libexec/gnubin" ]]; then
      export PATH="$prefix/opt/findutils/libexec/gnubin:$PATH"
    fi
    if [[ -d "$prefix/opt/gawk/libexec/gnubin" ]]; then
      export PATH="$prefix/opt/gawk/libexec/gnubin:$PATH"
    fi
  done

  # Create stat wrapper - converts GNU stat -c format to macOS stat -f format
  cat > "$CIUKIOS_WRAPPER_BIN/stat" << 'STAT_WRAPPER'
#!/usr/bin/env bash
# stat wrapper: converts GNU stat -c format to macOS stat -f format
format_arg="-f%A %p %l %Su %Sg %Hr %Xf %Sm %Sd %SH:%SM:%SS %SY %N"
files=()

i=0
while [[ $i -lt $# ]]; do
  arg="${!i}"
  ((i++))
  
  if [[ "$arg" =~ ^-c(.+)$ ]]; then
    fmt="${BASH_REMATCH[1]}"
    case "$fmt" in
      "%s") format_arg="-f%z" ;;  # file size
      "%Y") format_arg="-f%m" ;;  # modification time (seconds)
      "%X") format_arg="-f%a" ;;  # access time
      "%Z") format_arg="-f%c" ;;  # change time
      "%U") format_arg="-f%u" ;;  # uid
      "%G") format_arg="-f%g" ;;  # gid
      "%a") format_arg="-f%A" ;;  # permissions (symbolic)
      "%n") format_arg="-f%N" ;;  # filename
      *)    echo "stat: unsupported format: -c$fmt" >&2; exit 1 ;;
    esac
  elif [[ "$arg" =~ ^- ]]; then
    # Other flags - skip for now
    :
  else
    files+=("$arg")
  fi
done

/usr/bin/stat "$format_arg" "${files[@]}"
STAT_WRAPPER
  chmod +x "$CIUKIOS_WRAPPER_BIN/stat"

  # Create timeout wrapper (gtimeout if available)
  cat > "$CIUKIOS_WRAPPER_BIN/timeout" << 'TIMEOUT_WRAPPER'
#!/usr/bin/env bash
if command -v gtimeout >/dev/null 2>&1; then
  exec gtimeout "$@"
else
  # Fallback: just run without timeout
  shift  # Remove timeout value
  shift  # Remove the command
  "$@"
fi
TIMEOUT_WRAPPER
  chmod +x "$CIUKIOS_WRAPPER_BIN/timeout"
}

ciuk_macos_check_required() {
  local missing=0
  local cmd
  for cmd in bash nasm xxd dd stat grep awk sed timeout; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      echo "[macos-wrapper] missing required command: $cmd" >&2
      missing=1
    fi
  done

  if ! command -v qemu-system-i386 >/dev/null 2>&1 && ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "[macos-wrapper] missing required command: qemu-system-i386 (or qemu-system-x86_64)" >&2
    missing=1
  fi

  if [[ "$missing" -ne 0 ]]; then
    echo "[macos-wrapper] install toolchain first: bash scripts/macos/setup_macos26.sh --install" >&2
    exit 1
  fi
}
