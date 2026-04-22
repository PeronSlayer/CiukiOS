#!/usr/bin/env bash
set -euo pipefail

CIUKIOS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

ciuk_macos_prepare_tools() {
  local prefix
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
  done

  if ! command -v timeout >/dev/null 2>&1; then
    if command -v gtimeout >/dev/null 2>&1; then
      timeout() { gtimeout "$@"; }
      export -f timeout
    fi
  fi
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
