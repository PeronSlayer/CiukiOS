#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
probe_output="${1:-build/probes/doswindow}"
mkdir -p "$probe_output"
nasm -f bin src/probes/doswindow/text.asm -o "$probe_output/DWTEXT.COM" -l "$probe_output/DWTEXT-COM.lst"
nasm -f bin -D MZ=1 src/probes/doswindow/text.asm -o "$probe_output/DWTEXT.EXE" -l "$probe_output/DWTEXT-MZ.lst"
nasm -f bin -D BIOS_ONLY=1 src/probes/doswindow/text.asm -o "$probe_output/DWBIOST.COM" -l "$probe_output/DWBIOST-COM.lst"
nasm -f bin -D BIOS_ONLY=1 -D MZ=1 src/probes/doswindow/text.asm -o "$probe_output/DWBIOST.EXE" -l "$probe_output/DWBIOST-MZ.lst"
nasm -f bin src/probes/doswindow/child.asm -o "$probe_output/DWCHILD.COM" -l "$probe_output/DWCHILD.lst"
nasm -f bin -D BIOS_ONLY=1 src/probes/doswindow/child.asm -o "$probe_output/DWBIOSCH.COM" -l "$probe_output/DWBIOSCH.lst"
nasm -f bin src/probes/doswindow/performance.asm -o "$probe_output/DWPERF.COM" -l "$probe_output/DWPERF.lst"
sha256sum "$probe_output"/*.COM "$probe_output"/*.EXE
