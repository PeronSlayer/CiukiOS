#!/usr/bin/env bash
# scripts/test_opengem_file_browser.sh — OPENGEM-004 host-side static gate.
#
# Validates the app catalog module (FAT + handoff COM dedup) is
# wired in stage2, the `catalog` shell command exists and is
# referenced in help, and the frozen marker vocabulary is emitted
# from app_catalog.c. A runtime boot-log probe is opt-in.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HDR="$ROOT/stage2/include/app_catalog.h"
CSRC="$ROOT/stage2/src/app_catalog.c"
SHELL_SRC="$ROOT/stage2/src/shell.c"
STAGE2_SRC="$ROOT/stage2/src/stage2.c"
MAKEFILE="$ROOT/Makefile"
BOOT_LOG="$ROOT/.ciukios-testlogs/stage2-boot.log"

OK=0
FAIL=0

ok()  { echo "[OK] $1"; OK=$((OK+1)); }
ko()  { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

check_file() {
    [ -f "$1" ] && ok "file: ${1##*/}" || ko "file missing: $1"
}

check_contains() {
    local needle="$1" path="$2" label="$3"
    if grep -qF -- "$needle" "$path"; then ok "$label"
    else ko "$label (missing: $needle — file: $path)"; fi
}

echo "=== OPENGEM-004 app catalog smoke gate ==="

check_file "$HDR"
check_file "$CSRC"
check_file "$SHELL_SRC"
check_file "$STAGE2_SRC"
check_file "$MAKEFILE"

# Header contract
check_contains 'OPENGEM-004' "$HDR" "header: OPENGEM-004 marker"
check_contains 'APP_CATALOG_MAX_ENTRIES 256' "$HDR" "header: APP_CATALOG_MAX_ENTRIES=256"
check_contains 'APP_CATALOG_MAX_ROOTS' "$HDR" "header: APP_CATALOG_MAX_ROOTS"
check_contains 'APP_CATALOG_KIND_COM' "$HDR" "header: KIND_COM constant"
check_contains 'APP_CATALOG_KIND_EXE' "$HDR" "header: KIND_EXE constant"
check_contains 'APP_CATALOG_KIND_BAT' "$HDR" "header: KIND_BAT constant"
check_contains 'APP_CATALOG_SRC_FAT' "$HDR" "header: SRC_FAT constant"
check_contains 'APP_CATALOG_SRC_HANDOFF' "$HDR" "header: SRC_HANDOFF constant"
check_contains 'struct app_catalog_entry' "$HDR" "header: app_catalog_entry struct"
check_contains 'char name[13]' "$HDR" "header: name[13] field"
check_contains 'char path[64]' "$HDR" "header: path[64] field"
check_contains 'app_catalog_init' "$HDR" "header: app_catalog_init decl"
check_contains 'app_catalog_count' "$HDR" "header: app_catalog_count decl"
check_contains 'app_catalog_get' "$HDR" "header: app_catalog_get decl"
check_contains 'app_catalog_find' "$HDR" "header: app_catalog_find decl"
check_contains 'app_catalog_kind_label' "$HDR" "header: app_catalog_kind_label decl"

# Implementation: scan roots and marker vocabulary
check_contains '"/"' "$CSRC" "impl: scans root /"
check_contains '"/FREEDOS"' "$CSRC" "impl: scans root /FREEDOS"
check_contains '"/FREEDOS/OPENGEM"' "$CSRC" "impl: scans root /FREEDOS/OPENGEM"
check_contains '"/EFI/CiukiOS"' "$CSRC" "impl: scans root /EFI/CiukiOS"
check_contains '[ catalog ] scan begin root=' "$CSRC" "impl: marker scan begin"
check_contains '[ catalog ] scan entry ' "$CSRC" "impl: marker scan entry"
check_contains '[ catalog ] scan done entries=' "$CSRC" "impl: marker scan done"
check_contains 'fat_list_dir' "$CSRC" "impl: uses fat_list_dir"
check_contains 'handoff->com_entries' "$CSRC" "impl: reads handoff COM lane"

# stage2.c wiring
check_contains '#include "app_catalog.h"' "$STAGE2_SRC" "stage2.c: includes app_catalog.h"
check_contains 'app_catalog_init(handoff)' "$STAGE2_SRC" "stage2.c: calls app_catalog_init(handoff)"

# shell.c wiring: `catalog` command + help string
check_contains '#include "app_catalog.h"' "$SHELL_SRC" "shell.c: includes app_catalog.h"
check_contains 'str_eq(cmd, "catalog")' "$SHELL_SRC" "shell.c: dispatches 'catalog' command"
check_contains 'shell_cmd_catalog' "$SHELL_SRC" "shell.c: defines shell_cmd_catalog"
check_contains 'catalog  - list discovered apps' "$SHELL_SRC" "shell.c: help string for catalog"

# Makefile target
if grep -qE '^test-opengem-file-browser:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-file-browser target declared"
else
    ko "Makefile: test-opengem-file-browser target missing"
fi

# Opt-in boot-log probe
if [ -f "$BOOT_LOG" ]; then
    for m in \
        '[ catalog ] scan begin root=' \
        '[ catalog ] scan done entries='
    do
        if grep -qF -- "$m" "$BOOT_LOG"; then
            ok "boot-log: '$m'"
        else
            ko "boot-log: missing '$m'"
        fi
    done
else
    echo "[info] no boot log at $BOOT_LOG — skipping runtime marker probe"
fi

echo ""
echo "=== opengem-file-browser smoke summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM file-browser smoke gate"
    exit 1
fi
echo "[PASS] OpenGEM file-browser smoke gate"
