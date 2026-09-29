#!/usr/bin/env bash
# Complete test profile for the DOS-window VM session (CIUKIOS_VM_WINDOW=1).
#
#   scripts/test_vm_window_profile.sh [--image build/full/ciukios-full.img]
#                                     [--output build/tests/vm-window-profile]
#                                     [--quick]
#
# The session artifacts are taken from the image under test (C:\VM,
# \SBEMU\HDPMI32I.EXE, SYSTEM\SHELL.COM, SYSTEM\DOSWIN.DRV, SYSTEM\CIUKIDOS.SYS),
# so the gates exercise exactly what the image ships. Runs:
#   unit      peripheral model, virtual VGA, x86 differential (Unicorn),
#             scheduler (Unicorn), device link, DOS-window lifecycle (Unicorn),
#             Jemm device query (Unicorn)
#   v86       full-screen VGA session, legacy V86/DPMI sessions, V86 CLI/IRQ
#             profile, DEVTEST (keyboard/SB16/OPL on AC'97), HDPMI lifetime,
#             VGA DOS window (FIRE, resize, guest I/O, audio), text DOS window
#   dpmi      DPMIPORT probe; doom-vanille and the original DOOM in the DOS
#             window with music and with effects only (see
#             scripts/qemu_test_dpmi_window.py for the checks)
#   vmm       several DOS VMs (scripts/qemu_test_vmm.py): switching, a key
#             wait inside DOS, file I/O, a VM owning the DOS window session
#             (keyboard focus, Ctrl+Esc), SB16 DMA of a non-running VM,
#             exit/kill of a session owner, vectors of a forked VM, clocks,
#             two sessions at once (keyboard, mouse and AC'97 ownership
#             following the focus)
#   boot      firmware using 1 KiB of the caller's stack (INT 13h, PCI BIOS),
#             DOS memory allocation rules
# --quick runs only the unit suites and the dpmi group. Each gate writes its
# report.json under the output directory; SUMMARY.json lists every result.
# QEMU/KVM evidence only; nothing here qualifies physical hardware.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

IMAGE=build/full/ciukios-full.img
OUT=build/tests/vm-window-profile-$(date +%Y%m%d-%H%M%S)
QUICK=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--image) IMAGE="$2"; shift 2 ;;
		--output) OUT="$2"; shift 2 ;;
		--quick) QUICK=1; shift ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done
[[ -e "$OUT" ]] && { echo "[vm-profile] output exists (evidence is preserved): $OUT" >&2; exit 2; }
mkdir -p "$OUT/artifacts" "$OUT/logs"
A="$OUT/artifacts"
VOL="$IMAGE@@$(python3 -c "import sys;sys.path.insert(0,'scripts');from pathlib import Path;from qemu_test_installed_hdd import FAT16;print(FAT16(Path('$IMAGE')).start)")"
extract() { mcopy -o -n -i "$VOL" "::$1" "$A/$2" || { echo "[vm-profile] ERROR: $1 missing from $IMAGE" >&2; exit 1; }; }
extract VM/JEMM386.EXE JEMM386.EXE
extract VM/JLOAD.EXE JLOAD.EXE
extract VM/CVSESS.DLL CVSESSION.DLL
extract VM/DPMIRUN.COM DPMIRUN.COM
extract VM/VMFORK.COM VMFORK.COM
extract SBEMU/HDPMI32I.EXE HDPMI32I.EXE
extract SYSTEM/SHELL.COM SHELL.COM
extract SYSTEM/DOSWIN.DRV DOSWIN.DRV
extract SYSTEM/CIUKIDOS.SYS ciukidos.sys
# The UI harnesses read SHELL.COM symbols from its listing; it must be the
# listing of exactly the shipped binary.
nasm -f bin src/com/shell.asm -l "$A/shell.lst" -o "$A/SHELL-rebuilt.COM"
cmp -s "$A/SHELL.COM" "$A/SHELL-rebuilt.COM" \
	|| { echo "[vm-profile] ERROR: SYSTEM\\SHELL.COM on the image is not built from this tree" >&2; exit 1; }
bash scripts/build_dpmi_lifetime_probes.sh "$A/probes" > "$OUT/logs/probes.log" 2>&1 \
	|| { echo "[vm-profile] ERROR: probe build failed, see $OUT/logs/probes.log" >&2; exit 1; }
mkdir -p "$A/vmm"
for probe in vmmtest vmmchild vmmio vmwtest vmwchild vmstest vmschild vmktest vmkchild vmitest vmichild \
	vmctest vmcchild vmdtest vmotest vmochild vmatest; do
	nasm -f bin "src/probes/vm/$probe.asm" -o "$A/vmm/${probe^^}.COM" \
		|| { echo "[vm-profile] ERROR: VM-manager probe build failed" >&2; exit 1; }
done
cp "$A/VMFORK.COM" "$A/vmm/VMFORK.COM"
# The text DOS-window gate runs the DWBIOST probes; an image built without
# CIUKIOS_INCLUDE_DOS_WINDOW_PROBES=1 gets them in a private copy.
DOSWIN_IMAGE="$IMAGE"
if ! mdir -i "$VOL" ::APPS/DWBIOST.COM >/dev/null 2>&1; then
	bash src/probes/doswindow/build.sh "$A/doswindow" > "$OUT/logs/doswindow-probes.log" 2>&1 \
		|| { echo "[vm-profile] ERROR: DOS-window probe build failed" >&2; exit 1; }
	DOSWIN_IMAGE="$A/dos-window-probes.img"
	cp "$IMAGE" "$DOSWIN_IMAGE"
	PVOL="$DOSWIN_IMAGE@@${VOL##*@@}"
	for probe in DWBIOST.COM DWBIOST.EXE DWBIOSCH.COM; do
		mcopy -o -i "$PVOL" "$A/doswindow/$probe" "::APPS/$probe"
	done
fi
# Gates that load Jemm/CVSESSION themselves run on a copy whose desktop does
# not start the VM manager at boot (no \VM\VMSTART.COM).
MANUAL_IMAGE="$A/manual.img"
cp "$IMAGE" "$MANUAL_IMAGE"
mdel -i "$MANUAL_IMAGE@@${VOL##*@@}" ::VM/VMSTART.COM
if [[ "$DOSWIN_IMAGE" != "$IMAGE" ]]; then
	mdel -i "$DOSWIN_IMAGE@@${VOL##*@@}" ::VM/VMSTART.COM
fi
sha256sum "$IMAGE" "$A"/*.EXE "$A"/*.COM "$A"/*.DLL "$A"/*.DRV "$A"/ciukidos.sys > "$OUT/artifacts.sha256"

C=build/tests/vm-completion-2026-09-27          # fixed V86 test image and probes
UV=(uv run --no-project --with unicorn --with numpy --with pillow python)
declare -a NAMES=() PIDS=()
run() {  # run NAME command... (background)
	local name="$1"; shift
	( "$@" > "$OUT/logs/$name.log" 2>&1; echo $? > "$OUT/logs/$name.rc" ) &
	NAMES+=("$name"); PIDS+=($!)
}
drain() { wait; }

echo "[vm-profile] unit suites"
run unit-peripherals python3 scripts/test_guest_peripherals.py --output "$OUT/unit-peripherals"
run unit-virtual-vga python3 scripts/test_virtual_vga.py
run unit-vga-x86 "${UV[@]}" scripts/test_vga_x86.py --cases 20000
run unit-scheduler "${UV[@]}" scripts/test_session_scheduler.py --output "$OUT/unit-scheduler"
run unit-device-link python3 scripts/test_vm_device_link.py --output "$OUT/unit-device-link"
run unit-lifecycle "${UV[@]}" scripts/test_dos_window_lifecycle.py --output "$OUT/unit-lifecycle"
run unit-jemm-query "${UV[@]}" scripts/test_jemm_device_query.py
drain

J=(--jemm "$A/JEMM386.EXE" --jload "$A/JLOAD.EXE" --module "$A/CVSESSION.DLL")
WINDOW=(--image "$MANUAL_IMAGE" --kernel "$A/ciukidos.sys" --shell "$A/SHELL.COM" --listing "$A/shell.lst"
        --runtime "$A/DOSWIN.DRV" "${J[@]}")
if [[ "$QUICK" != "1" ]]; then
	echo "[vm-profile] V86 gates"
	run vga-session timeout 1500 python3 scripts/qemu_test_vga_session.py --image "$MANUAL_IMAGE" --kernel "$A/ciukidos.sys" "${J[@]}" --output "$OUT/vga-session"
	run legacy-v86 timeout 900 python3 scripts/qemu_test_vm_session.py --image "$MANUAL_IMAGE" --kernel "$A/ciukidos.sys" "${J[@]}" --output "$OUT/legacy-v86"
	run legacy-dpmi timeout 900 python3 scripts/qemu_test_vm_session.py --image "$MANUAL_IMAGE" --kernel "$A/ciukidos.sys" "${J[@]}" --dpmi-probe "$C/dpmi-probe/DPMIVGA.EXE" --output "$OUT/legacy-dpmi"
	drain
	run v86cli timeout 900 python3 scripts/qemu_test_v86_cli.py --image "$C/input.img" --kernel "$A/ciukidos.sys" "${J[@]}" --output "$OUT/v86cli"
	run devtest timeout 900 python3 scripts/qemu_test_devices.py --image "$C/input.img" --kernel "$A/ciukidos.sys" "${J[@]}" --output "$OUT/devtest"
	run lifetime timeout 1800 python3 scripts/qemu_test_dpmi_lifetime.py --image "$C/input.img" "${J[@]}" --hdpmi "$A/HDPMI32I.EXE" --life "$A/probes/DPMILIF.EXE" --fault "$A/probes/DPMIFLT.EXE" --output "$OUT/lifetime"
	drain
	run vga-window timeout 2400 python3 scripts/qemu_test_vga_window.py "${WINDOW[@]}" --output "$OUT/vga-window"
	run dos-window-text timeout 1800 python3 scripts/qemu_test_dos_window.py --image "$DOSWIN_IMAGE" --listing "$A/shell.lst" --output "$OUT/dos-window-text"
	drain
fi

echo "[vm-profile] DPMI window gates"
DPMI=(python3 scripts/qemu_test_dpmi_window.py "${WINDOW[@]}" --hdpmi "$A/HDPMI32I.EXE" --launcher "$A/DPMIRUN.COM")
# Main-build flow: VM manager started at boot, game typed into Run, its DPMI
# host comes with the DOS window.
AUTO=(python3 scripts/qemu_test_dpmi_window.py --image "$IMAGE" --kernel "$A/ciukidos.sys" --shell "$A/SHELL.COM"
      --listing "$A/shell.lst" --runtime "$A/DOSWIN.DRV" --jemm - --jload - --module - --hdpmi - --auto)
run dpmi-probe timeout 900 "${DPMI[@]}" --probe "$A/probes/DPMIPORT.EXE" --output "$OUT/dpmi-probe"
run doom-vanille timeout 1800 "${DPMI[@]}" --samples 6 --output "$OUT/doom-vanille"
drain
run doom-original timeout 1800 "${AUTO[@]}" --samples 6 --program '\APPS\DOOM\DOOMCORE.EXE' --output "$OUT/doom-original"
run doom-vanille-sfx timeout 1800 "${AUTO[@]}" --samples 6 --game-args '-warp 1 1 -nomusic' --output "$OUT/doom-vanille-sfx"
drain
run doom-original-sfx timeout 1800 "${AUTO[@]}" --samples 6 --program '\APPS\DOOM\DOOMCORE.EXE' --game-args '-warp 1 1 -nomusic' --output "$OUT/doom-original-sfx"
if [[ "$QUICK" != "1" ]]; then
	run multi-vm timeout 1800 python3 scripts/qemu_test_vmm.py --image "$IMAGE" "${J[@]}" --probes "$A/vmm" --output "$OUT/multi-vm"
	drain
	# Boot with firmware that uses 1 KiB of the caller's stack in INT 13h
	# and the PCI BIOS; DOS memory rules, also in a nested child and a VM.
	run startup-stack timeout 900 python3 scripts/qemu_test_startup_stack.py --image "$IMAGE" --fault both --output "$OUT/startup-stack"
	run dos-memory timeout 900 python3 scripts/qemu_test_full_dos_memory.py --image "$IMAGE" --output "$OUT/dos-memory"
fi
drain

python3 - "$OUT" "${NAMES[@]}" <<'PY'
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
rows = []
for name in sys.argv[2:]:
    rc = int((out / 'logs' / f'{name}.rc').read_text().strip() or 1)
    report = out / name / 'report.json'
    passed = None
    if report.exists():
        try:
            passed = json.loads(report.read_text()).get('passed')
        except ValueError:
            passed = None
    ok = rc == 0 and passed is not False
    tail = (out / 'logs' / f'{name}.log').read_text(errors='replace').strip().splitlines()[-1:]
    rows.append(dict(gate=name, ok=ok, exit=rc, report_passed=passed, last_line=tail[0][:200] if tail else ''))
summary = dict(passed=all(r['ok'] for r in rows), gates=rows, physical_hardware_qualified=False)
(out / 'SUMMARY.json').write_text(json.dumps(summary, indent=2) + '\n')
for r in rows:
    print(f"{'PASS' if r['ok'] else 'FAIL'}  {r['gate']:<20} {r['last_line']}")
print('[vm-profile] ' + ('ALL PASS' if summary['passed'] else 'FAILURES') + f' -> {out}/SUMMARY.json')
sys.exit(0 if summary['passed'] else 1)
PY
