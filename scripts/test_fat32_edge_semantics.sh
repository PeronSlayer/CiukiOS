#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAT_FILE="$PROJECT_DIR/stage2/src/fat.c"

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
    local pattern="$1"
    grep -Fq "$pattern" "$FAT_FILE"
}

echo "=== FAT32 Edge Semantics Gate v1 ==="

if require_pattern "rd32(sector + 0x000U) != FAT32_FSINFO_LEAD_SIG" &&
   require_pattern "rd32(sector + 0x1E4U) != FAT32_FSINFO_STRUCT_SIG" &&
   require_pattern "rd32(sector + 0x1FCU) != FAT32_FSINFO_TRAIL_SIG" &&
   require_pattern "g_fs.fsinfo_valid = 0U;"; then
    gate "invalid FSInfo signatures disable FSInfo-backed state" 0
else
    gate "invalid FSInfo signatures disable FSInfo-backed state" 1
fi

if require_pattern "g_fs.next_free_hint = fat_sanitize_cluster_hint(rd32(fsinfo + 0x1ECU));" &&
   require_pattern "g_fs.next_free_hint = fat_sanitize_cluster_hint(g_fs.next_free_hint);"; then
    gate "next_free_hint is sanitized both on mount and steady state" 0
else
    gate "next_free_hint is sanitized both on mount and steady state" 1
fi

if require_pattern "if (g_fs.next_free_hint < 2U || min_freed < g_fs.next_free_hint)" &&
   require_pattern "g_fs.next_free_hint = min_freed;"; then
    gate "free path lowers next_free_hint after reclaimed clusters" 0
else
    gate "free path lowers next_free_hint after reclaimed clusters" 1
fi

fsinfo_sync_count=$(grep -Fc "(void)fat_fsinfo_sync(g_fs.next_free_hint);" "$FAT_FILE" || true)
if [[ "$fsinfo_sync_count" -ge 2 ]]; then
    gate "allocation and free paths both sync FSInfo metadata" 0
else
    gate "allocation and free paths both sync FSInfo metadata" 1
fi

if require_pattern "/* C3: guard against zero cluster size (corrupt geometry) */"; then
    gate "corrupt geometry guard prevents zero-cluster-size writes" 0
else
    gate "corrupt geometry guard prevents zero-cluster-size writes" 1
fi

if require_pattern "return 0; /* root directory full */"; then
    gate "fixed-root overflow remains guarded for FAT12/16 semantics" 0
else
    gate "fixed-root overflow remains guarded for FAT12/16 semantics" 1
fi

if require_pattern "out->fsinfo_sector = g_fs.fsinfo_sector;" &&
   require_pattern "out->fsinfo_valid = g_fs.fsinfo_valid;" &&
   require_pattern "out->next_free_hint = fat_sanitize_cluster_hint(g_fs.next_free_hint);"; then
    gate "FAT info export exposes sanitized FAT32 edge-state diagnostics" 0
else
    gate "FAT info export exposes sanitized FAT32 edge-state diagnostics" 1
fi

if make -C "$PROJECT_DIR" all >/dev/null 2>&1; then
    gate "make all compiles with FAT32 edge handling" 0
else
    gate "make all compiles with FAT32 edge handling" 1
fi

echo "=== SUMMARY: PASS=$pass FAIL=$fail ==="
if [[ "$fail" -ne 0 ]]; then
    exit 1
fi
