#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHELL_FILE="$PROJECT_DIR/stage2/src/shell.c"
RUN_FILE="$PROJECT_DIR/run_ciukios.sh"

pass=0
fail=0

gate() {
    local desc="$1"
    local rc="$2"
    if [[ "$rc" -eq 0 ]]; then
        echo "[PASS] $desc"
        pass=$((pass + 1))
    else
        echo "[FAIL] $desc"
        fail=$((fail + 1))
    fi
}

require_pattern() {
    local file="$1"
    local pattern="$2"
    grep -Fq "$pattern" "$file"
}

echo "=== Startup Chain Gate v1 ==="

if require_pattern "$SHELL_FILE" "shell_startup_chain(boot_info, handoff);"; then
    gate "startup chain invoked before interactive shell loop" 0
else
    gate "startup chain invoked before interactive shell loop" 1
fi

if require_pattern "$SHELL_FILE" "shell_process_config_sys();" &&
   require_pattern "$SHELL_FILE" "fat_find_file(\"/AUTOEXEC.BAT\", &info)" &&
   require_pattern "$SHELL_FILE" "shell_run_batch_file(boot_info, handoff, \"/AUTOEXEC.BAT\")"; then
    gate "CONFIG.SYS and AUTOEXEC.BAT startup chain wiring present" 0
else
    gate "CONFIG.SYS and AUTOEXEC.BAT startup chain wiring present" 1
fi

if require_pattern "$SHELL_FILE" "shell_env_set(\"COMSPEC\", \"COMMAND.COM\")" &&
   require_pattern "$SHELL_FILE" "shell_env_set(\"PATH\", \"\\\\;\\\\FREEDOS\")"; then
    gate "default COMSPEC and PATH startup environment seeded" 0
else
    gate "default COMSPEC and PATH startup environment seeded" 1
fi

if require_pattern "$SHELL_FILE" "if (str_starts_with_nocase(line, \"shell=\"))" &&
   require_pattern "$SHELL_FILE" "shell_env_set(\"COMSPEC\", v);" &&
   require_pattern "$SHELL_FILE" "else if (str_starts_with_nocase(line, \"set \"))"; then
    gate "CONFIG.SYS parser supports shell override and SET directives" 0
else
    gate "CONFIG.SYS parser supports shell override and SET directives" 1
fi

if require_pattern "$SHELL_FILE" "shell_batch_find_label" &&
   require_pattern "$SHELL_FILE" "if (str_starts_with_nocase(expanded, \"goto \"))" &&
   require_pattern "$SHELL_FILE" "if (str_starts_with_nocase(expanded, \"if errorlevel \"))"; then
    gate "batch parser supports labels, GOTO and IF ERRORLEVEL" 0
else
    gate "batch parser supports labels, GOTO and IF ERRORLEVEL" 1
fi

if require_pattern "$SHELL_FILE" "shell_env_expand_line(line, expanded, (u32)sizeof(expanded));" &&
   require_pattern "$SHELL_FILE" "if (str_starts_with_nocase(expanded, \"set \"))" &&
   require_pattern "$SHELL_FILE" "if (str_starts_with_nocase(expanded, \"echo \"))" &&
   require_pattern "$SHELL_FILE" "Batch recursion limit reached."; then
    gate "batch runtime supports env expansion, SET/ECHO and recursion guard" 0
else
    gate "batch runtime supports env expansion, SET/ECHO and recursion guard" 1
fi

if require_pattern "$SHELL_FILE" "shell_run_batch_file(boot_info, handoff, target_path);"; then
    gate "run command dispatches BAT files through batch runtime" 0
else
    gate "run command dispatches BAT files through batch runtime" 1
fi

if require_pattern "$RUN_FILE" 'copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/FDCONFIG.SYS" ::FDCONFIG.SYS' &&
    require_pattern "$RUN_FILE" 'copy_freedos_file_if_present "$FREEDOS_RUNTIME_DIR/FDAUTO.BAT" ::AUTOEXEC.BAT'; then
    gate "runtime image wiring maps FreeDOS startup files into DOS root" 0
else
    gate "runtime image wiring maps FreeDOS startup files into DOS root" 1
fi

if make -C "$PROJECT_DIR" all >/dev/null 2>&1; then
    gate "make all compiles with startup-chain support" 0
else
    gate "make all compiles with startup-chain support" 1
fi

echo "=== SUMMARY: PASS=$pass FAIL=$fail ==="
if [[ "$fail" -ne 0 ]]; then
    exit 1
fi
