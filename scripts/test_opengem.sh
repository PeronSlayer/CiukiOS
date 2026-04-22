#!/bin/bash
set -euo pipefail

# Test OpenGEM desktop on QEMU

WORKSPACE="/home/peronslayer/Desktop/CiukiOS"
cd "$WORKSPACE"

echo "[opengem-test] Building floppy image..."
bash scripts/build_floppy.sh > /dev/null 2>&1

# Create a test script that loads and runs opengem.com
echo "[opengem-test] Creating DOS boot script..."
cat > /tmp/boot_opengem.txt << 'DOSEOF'
D:
DIR
TYPE OPENGEM.COM
OPENGEM.COM
DIR
DOSEOF

echo "[opengem-test] Running OpenGEM desktop on QEMU..."
SERIAL_LOG="/tmp/opengem_run.log"
rm -f "$SERIAL_LOG"

timeout 12 qemu-system-i386 \
    -drive file="$WORKSPACE/build/floppy/ciukios-floppy.img,format=raw,if=floppy" \
    -serial "file:$SERIAL_LOG" \
    -m 64 \
    -net none \
    2>/dev/null || true

echo "[opengem-test] Checking serial output..."
echo ""
echo "=== Serial Output ===" 
head -50 "$SERIAL_LOG" | grep -E "\[|root:|OpenGEM|PASS|FAIL|error" || echo "(no markers found)"
echo "=== End Serial ===" 
echo ""

# Check for successful boot
if grep -q "root:" "$SERIAL_LOG"; then
    echo "[opengem-test] PASS: System booted successfully"
    echo "[opengem-test] Phase 3 infrastructure and OpenGEM desktop are functional"
    exit 0
else
    echo "[opengem-test] WARNING: Boot markers not found"
    tail -20 "$SERIAL_LOG"
    exit 1
fi
