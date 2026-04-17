#!/usr/bin/env bash
# Deterministic static gate for the VGA mode 13h readiness baseline.
# Validates that the compatibility scaffold (marker + shell command + docs)
# is wired. Full runtime mode-13h draw/render validation is deferred to the
# DOOM graphics step and will replace this static gate.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() {
	echo "[FAIL] $1" >&2
	exit 1
}

grep -Fq 'VGA mode 13h baseline v0 (compatibility scaffold):' \
	"$PROJECT_DIR/stage2/src/shell.c" || fail "shell_vga13_baseline text missing"

grep -Fq 'if (str_eq(cmd, "vga13"))' \
	"$PROJECT_DIR/stage2/src/shell.c" || fail "vga13 shell command not wired"

grep -Fq 'm6_vga13_baseline_ready' \
	"$PROJECT_DIR/stage2/src/stage2.c" || fail "vga13 readiness marker missing"

grep -Fq '[compat] vga13 baseline ready (320x200x8 scaffold)' \
	"$PROJECT_DIR/stage2/src/stage2.c" || fail "vga13 startup marker string missing"

grep -Fq '[compat] bios int10 baseline ready' \
	"$PROJECT_DIR/stage2/src/stage2.c" || fail "bios int10 compat marker missing"

grep -Fq '[compat] bios int16 baseline ready' \
	"$PROJECT_DIR/stage2/src/stage2.c" || fail "bios int16 compat marker missing"

grep -Fq '[compat] bios int1a baseline ready' \
	"$PROJECT_DIR/stage2/src/stage2.c" || fail "bios int1a compat marker missing"

echo "[PASS] vga13 baseline markers + shell command wired"
