#!/usr/bin/env bash
# OPENGEM-002-BAT — static smoke gate for the BAT interpreter
# hardening phase. Asserts that the stage2 BAT interpreter exposes
# the expected keyword surface, that fixture BATs exist on disk, and
# that the Makefile exposes the `test-bat-interp` target. Fails with
# exit code 1 on the first [FAIL] assertion.
#
# This gate intentionally stays host-side and static. A runtime boot
# probe is attempted opportunistically when a captured boot log is
# present under .ciukios-testlogs/stage2-boot.log; otherwise the
# runtime section is reported as SKIP.

set -u
set -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHELL_SRC="$ROOT/stage2/src/shell.c"
FIXTURE_DIR="$ROOT/tests/bat"
MAKEFILE="$ROOT/Makefile"
BOOT_LOG="$ROOT/.ciukios-testlogs/stage2-boot.log"

pass=0
fail=0

ok() {
    echo "[OK] $1"
    pass=$((pass + 1))
}

ko() {
    echo "[FAIL] $1"
    fail=$((fail + 1))
}

check_contains() {
    local needle="$1"
    local file="$2"
    local label="$3"
    if grep -qF -- "$needle" "$file"; then
        ok "$label"
    else
        ko "$label (missing: $needle — file: $file)"
    fi
}

check_exists() {
    local path="$1"
    local label="$2"
    if [ -e "$path" ]; then
        ok "$label"
    else
        ko "$label (missing: $path)"
    fi
}

# --- Stage2 interpreter keyword surface ---------------------------------
check_contains 'OPENGEM-002-BAT' "$SHELL_SRC" "stage2: OPENGEM-002-BAT marker present"
check_contains 'SHELL_BATCH_ARGV_MAX' "$SHELL_SRC" "stage2: batch argv table declared"
check_contains 'g_batch_echo' "$SHELL_SRC" "stage2: batch echo state declared"
check_contains 'g_batch_argc' "$SHELL_SRC" "stage2: batch argc declared"
check_contains 'g_batch_argv' "$SHELL_SRC" "stage2: batch argv declared"
check_contains 'g_batch_cur_path' "$SHELL_SRC" "stage2: batch current-path declared"

check_contains '%% -> literal' "$SHELL_SRC" "stage2: %% literal expansion implemented"
check_contains '%0..%9 -> batch positional arg' "$SHELL_SRC" "stage2: %0..%9 positional expansion implemented"

check_contains '"echo off"' "$SHELL_SRC" "stage2: ECHO OFF keyword handled"
check_contains '"echo on"' "$SHELL_SRC" "stage2: ECHO ON keyword handled"
check_contains '"echo."' "$SHELL_SRC" "stage2: ECHO. blank-line keyword handled"
check_contains '"shift"' "$SHELL_SRC" "stage2: SHIFT keyword handled"
check_contains '"pause"' "$SHELL_SRC" "stage2: PAUSE keyword handled"
check_contains '"call "' "$SHELL_SRC" "stage2: CALL keyword handled"
check_contains 'GOTO :EOF' "$SHELL_SRC" "stage2: GOTO :EOF support documented/implemented"
check_contains '"if "' "$SHELL_SRC" "stage2: IF dispatch block present"
check_contains '"exist "' "$SHELL_SRC" "stage2: IF EXIST handled"
check_contains '"errorlevel "' "$SHELL_SRC" "stage2: IF ERRORLEVEL handled"
check_contains 'leading `@`' "$SHELL_SRC" "stage2: leading @ echo-suppression documented"

# --- Serial marker vocabulary --------------------------------------------
check_contains '[ bat ] enter ' "$SHELL_SRC" "stage2: marker [ bat ] enter"
check_contains '[ bat ] exit ' "$SHELL_SRC" "stage2: marker [ bat ] exit"
check_contains '[ bat ] line: ' "$SHELL_SRC" "stage2: marker [ bat ] line"
check_contains '[ bat ] call ' "$SHELL_SRC" "stage2: marker [ bat ] call"
check_contains '[ bat ] return' "$SHELL_SRC" "stage2: marker [ bat ] return"
check_contains '[ bat ] goto ' "$SHELL_SRC" "stage2: marker [ bat ] goto"
check_contains '[ bat ] goto :eof' "$SHELL_SRC" "stage2: marker [ bat ] goto :eof"
check_contains '[ bat ] pause' "$SHELL_SRC" "stage2: marker [ bat ] pause"
check_contains '[ bat ] shift' "$SHELL_SRC" "stage2: marker [ bat ] shift"
check_contains '[ bat ] aborted max-steps' "$SHELL_SRC" "stage2: marker [ bat ] aborted"
check_contains 'gem.bat reached gemvdi invocation' "$SHELL_SRC" "stage2: marker gem.bat reached gemvdi"

# --- Fixtures ------------------------------------------------------------
check_exists "$FIXTURE_DIR/minimal.bat" "fixture: minimal.bat present"
check_exists "$FIXTURE_DIR/args.bat" "fixture: args.bat present"
check_exists "$FIXTURE_DIR/flow.bat" "fixture: flow.bat present"
check_exists "$FIXTURE_DIR/pause-skip.bat" "fixture: pause-skip.bat present"

check_contains '@echo off' "$FIXTURE_DIR/minimal.bat" "fixture minimal.bat: @echo off"
if grep -qiE '^[[:space:]]*shift$' "$FIXTURE_DIR/args.bat"; then
    ok "fixture args.bat: SHIFT keyword present"
else
    ko "fixture args.bat: SHIFT keyword missing"
fi
check_contains 'goto :eof' "$FIXTURE_DIR/flow.bat" "fixture flow.bat: goto :eof"
check_contains 'if exist' "$FIXTURE_DIR/flow.bat" "fixture flow.bat: IF EXIST"
check_contains 'call ' "$FIXTURE_DIR/flow.bat" "fixture flow.bat: CALL"
check_contains 'pause' "$FIXTURE_DIR/pause-skip.bat" "fixture pause-skip.bat: PAUSE keyword"

# --- Makefile target -----------------------------------------------------
check_contains 'test-bat-interp:' "$MAKEFILE" "Makefile: test-bat-interp target declared"

# --- Runtime boot-log probe (opt-in) -------------------------------------
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present, probing batch markers..."
    if grep -qF '[ bat ] enter ' "$BOOT_LOG"; then
        ok "runtime: boot log contains a [ bat ] enter marker"
    else
        echo "[info] boot log did not exercise a batch (no enter marker) — not a failure"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — skipping runtime marker probe"
fi

echo
echo "=== bat-interp smoke summary: $pass OK / $fail FAIL ==="
if [ "$fail" -gt 0 ]; then
    echo "[FAIL] BAT interpreter smoke gate"
    exit 1
fi
echo "[PASS] BAT interpreter smoke gate"
