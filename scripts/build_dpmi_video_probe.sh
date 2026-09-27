#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
VM_PROBE_OUT="${1:-build/tests/dpmi-video}"
mkdir -p "$VM_PROBE_OUT"
VM_PROBE_OUT="$(cd "$VM_PROBE_OUT" && pwd)"
WATCOM_ROOT="${WATCOM:-/opt/watcom}"
export WATCOM="$WATCOM_ROOT"
export INCLUDE="$WATCOM_ROOT/h"
export PATH="$WATCOM_ROOT/binl64:$WATCOM_ROOT/binl:$PATH"
nasm -f obj src/vm/dpmi_video_io.asm -o "$VM_PROBE_OUT/dpmi_video_io.obj"
nasm -f obj src/vm/dpmi_video_fault_thunk.asm -o "$VM_PROBE_OUT/dpmi_video_fault_thunk.obj"
wcc386 -zq -bt=dos -mf -3r -ecc -s -w4 -we -fo="$VM_PROBE_OUT/vga_x86.obj" src/vm/vga_x86.c
wcc386 -zq -bt=dos -mf -3r -ecc -s -w4 -we -fo="$VM_PROBE_OUT/dpmi_video_fault.obj" src/vm/dpmi_video_fault.c
wcc386 -zq -bt=dos -mf -3r -ecc -s -w4 -we -zl \
    -fo="$VM_PROBE_OUT/virtual_vga.obj" src/vm/virtual_vga.c
wcc386 -zq -bt=dos -mf -3r -ecc -s -w4 -we \
    -fo="$VM_PROBE_OUT/dpmi_video.obj" src/probes/vm/dpmi_video.c
wlink option quiet,map="$VM_PROBE_OUT/DPMIVGA.map" system dos4g name "$VM_PROBE_OUT/DPMIVGA.EXE" \
    option stack=32768 file "$VM_PROBE_OUT/dpmi_video.obj" \
    file "$VM_PROBE_OUT/dpmi_video_io.obj" file "$VM_PROBE_OUT/virtual_vga.obj" \
    file "$VM_PROBE_OUT/vga_x86.obj" file "$VM_PROBE_OUT/dpmi_video_fault.obj" \
    file "$VM_PROBE_OUT/dpmi_video_fault_thunk.obj"
printf 'Built actual HDPMI I/O probe: %s\n' "$VM_PROBE_OUT/DPMIVGA.EXE"
