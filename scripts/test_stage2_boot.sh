#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/stage2-boot.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"
DEBUGCON_LOG="$PROJECT_DIR/build/debugcon.log"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-420}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE"

# Avoid parallel QEMU/image races with other boot tests.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE"
    if ! flock -w 180 9; then
        echo "[FAIL] could not acquire QEMU test lock: $LOCK_FILE" >&2
        exit 1
    fi
fi

if [[ ! -x "$RUN_SCRIPT" ]]; then
    echo "[FAIL] run script not found or not executable: $RUN_SCRIPT" >&2
    exit 1
fi

echo "[test-stage2] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-stage2] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
CIUKIOS_INCLUDE_FREEDOS=0 \
CIUKIOS_INCLUDE_OPENGEM=0 \
CIUKIOS_SKIP_BUILD=1 \
CIUKIOS_QEMU_HEADLESS=1 \
timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
rc=$?
set -e

# Fast diagnostic: if QEMU launched but produced no loader/stage2 serial markers,
# fail with infra-focused guidance instead of a generic missing-pattern error.
serial_marker_count=$(grep -Ec "stage2\.elf loaded into memory|Stage2 ELF loaded, leaving Boot Services|\[ stage2 \]|COM catalog ready:" "$LOG_FILE" || true)
if [[ "$serial_marker_count" -eq 0 ]]; then
    if grep -Fq "[CiukiOS] Starting QEMU..." "$LOG_FILE"; then
        echo "[INFRA] no loader/stage2 serial markers captured after QEMU launch." >&2
        if [[ -f "$DEBUGCON_LOG" ]]; then
            if grep -q . "$DEBUGCON_LOG"; then
                echo "[INFRA] debugcon tail ($DEBUGCON_LOG):" >&2
                tail -n 120 "$DEBUGCON_LOG" >&2 || true
            else
                echo "[INFRA] debugcon log exists but is empty: $DEBUGCON_LOG" >&2
            fi
        else
            echo "[INFRA] debugcon log not found: $DEBUGCON_LOG" >&2
        fi
        echo "[INFRA] stage2 gate cannot classify runtime behavior on this host (serial capture unavailable)." >&2
        exit 1
    fi
fi

if [[ $rc -eq 124 ]]; then
    echo "[test-stage2] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "stage2.elf loaded into memory"
    "COM catalog ready:"
    "Disk cache ready: lba_count="
    "Stage2 ELF loaded, leaving Boot Services"
    "[ stage2 ] scaffolding started"
    "[ ok ] boot_info is valid"
    "[ ok ] handoff v0 is valid"
    "[ ok ] stage2 local gdt+tss is active"
    "[ ok ] stage2 local idt is active"
    "[ ok ] pic remapped and pit started"
    "[ ok ] keyboard ring buffer + set1 decoder ready"
    "[ ok ] interrupts enabled (timer irq0 + keyboard irq1)"
    "[ compat ] INT10h baseline path ready (stage2 video text/gfx)"
    "[ compat ] INT16h baseline path ready (irq1 + key buffer)"
    "[ compat ] INT1Ah baseline path ready (pit tick source)"
    "[ compat ] INT21h PSP/status path ready (AH=51h/62h/4Dh)"
    "[ compat ] INT21h console/dta/drive ready (AH=06h/07h/0Ah/0Eh/1Ah/2Fh)"
    "[ compat ] INT21h io/handle baseline ready (AH=0Bh/0Ch/3Ch..43h)"
    "[ compat ] INT21h memory api ready (AH=48h/49h/4Ah)"
    "[ test ] phase2 timer tick progress: PASS"
    "[ test ] phase2 keyboard decode/capture: PASS"
    "[ test ] phase2 low-level core selftest: PASS"
    "[ test ] int21 priority-a selftest: PASS"
    "[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/ticks/mem/run/opengem/vmode/shutdown/reboot)"
    "[ tick ] irq0 #0000000000000001"
    "[ ok ] splashscreen rendered src=0x"
    "[ ui ] boot hud active"
    "[ compat ] INT21h FAT-backed file handles ready (AH=3Ch/3Dh/3Eh/3Fh/40h/41h/42h/43h/56h)"
    "[ test ] int21 fat-handle e2e selftest: PASS"
    "[ compat ] INT21h file search ready (AH=4Eh/4Fh)"
    "[ test ] int21 findfirst/findnext selftest: PASS"
)

forbidden_patterns=(
    "Invalid Opcode"
    "#UD"
    "Can't find image information"
    "[ tick ] irq0 #0000000000000064"
    "[ test ] phase2 timer tick progress: FAIL"
    "[ test ] phase2 keyboard decode/capture: FAIL"
    "[ test ] phase2 low-level core selftest: FAIL"
    "[ test ] int21 priority-a selftest: FAIL"
    "[ test ] int21 fat-handle e2e selftest: FAIL"
    "[ panic ]"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] missing required pattern: $pattern" >&2
        tail -n 160 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] forbidden pattern found: $pattern" >&2
        tail -n 160 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] absent: $pattern"
done

echo "[PASS] stage2 boot test completed"
echo "[INFO] log: $LOG_FILE"
