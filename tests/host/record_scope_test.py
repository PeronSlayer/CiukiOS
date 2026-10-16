#!/usr/bin/env python3
"""Evidence sinks are owned by probes, never by boot/device services."""
from pathlib import Path
import re
import sys

# Explicit ownership list: adding a producer requires a reviewed probe file.
ALLOWED = {
    'core/output.c',
    'proc/supervisor.c',   # controller framing of application records, post-BEGIN by construction (lead decision 2026-10-11)
    'probes/probes.c',
    'probes/selector.c',
    'probes/registry_probe.c',
    'probes/safe_probe.c',
    'probes/fat_probes.c',
    'probes/f4_probes_media.c',
    'probes/f2_probes_process.c',
    'probes/f2_probes_signals.c',
    'probes/f2_probes_desktop.c',
    'probes/f2_probes_app.c',
    'probes/f2_probes_files.c',
    'probes/f4_probes_audio.c', # f4-01 native audio controller
    'probes/f5_probes_core.c', # f5-03 personality mechanism controller
    'probes/f6_probes_net.c', # f6-01 bounded ARP controller
    'probes/f4_probes_vm_audio.c', # f4-05 DOS VM audio output controller (f4:vm-audio)
    'probes/f3_probes_vm.c',
    'probes/f3_probes_devices.c', # f3-01 DOS device controller (f3:vm-devices)
    'probes/f3_probes_video.c', # f3-02 virtual VGA stage digests
    'probes/f3_probes_audio_state.c', # f3-05 SB16/OPL3/8237 controller (f3:vm-audio-state)
    'probes/f3_probes_dpmi.c', # f3-04 DPMI host row (f3:dpmi)
    'probes/f3_probes_workloads.c', # f3-06 workloads (f3:dos4gw, doom, doom-sound, wolf3d, vm-starve)
    'vm/dosvm_perf.inc', # bounded protected live rows and droppable stop-time history
    'vm/dosvm.c',         # f3-00 admission/lifecycle reports, guarded by active probe scope
    'vm/dosvm_bios.c',    # f3-00 bounded, escaped DOS record channel
    'drivers/ata_probe.c',
    'drivers/fbdev_probe.c',
    'drivers/i8042_probe.c',
}
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
# Disk publication policy; runtime sink tests verify these at the output boundary.
PUBLICATION_CLASSES = {
    'rec_emit': 'protected',
    'rec_emit_panicsafe': 'panic_raw',
    'rec_diag': 'verbose',
    'rec_diag_disk': 'protected',
    'rec_diag_disk_bootlog': 'protected',
}
LOG_CLASSES = {'klog': 'protected', 'klog_periodic': 'protected', 'klog_diag': 'verbose'}
CALL = re.compile(r'\b(?:rec_emit(?:_panicsafe)?|rec_diag(?:_disk(?:_bootlog)?)?)\s*\(')


def calls(source):
    clean = TOKEN.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source)
    for match in CALL.finditer(clean):
        # Public prototypes are declarations, rather than evidence calls.
        if re.search(r'\bvoid\s*$', clean[:match.start()]):
            continue
        yield clean.count('\n', 0, match.start()) + 1


assert list(calls('/* rec_emit(p, e, 0); */ "rec_emit(p, e, 0)";')) == []
assert list(calls('void rec_emit(const char *p);\nrec_emit\n(p,e,0);')) == [2]
assert list(calls('rec_emit_panicsafe(p, e, 0);')) == [1]
assert list(calls('rec_diag_disk(p, "group=vm_perf_live");')) == [1]
assert list(calls('rec_diag_disk_bootlog(p, "group=vm_perf_cpu");')) == [1]
root = Path(__file__).resolve().parents[2] / 'src/kernel'
violations = []
for path in sorted(root.rglob('*')):
    if path.suffix not in {'.c', '.h', '.inc'} or path.relative_to(root).as_posix() in ALLOWED:
        continue
    violations.extend(f'{path.relative_to(root)}:{line}' for line in calls(path.read_text()))
if violations:
    print('FAIL evidence record scope: ' + ', '.join(violations), file=sys.stderr)
    sys.exit(1)
# Check class ownership independently of record group strings: the live and
# historical rows deliberately reuse vm_perf names.
perf = (root / 'vm/dosvm_perf.inc').read_text()
live = perf[perf.index('static void perf_live('):perf.index('void dosvm_perf_report(')]
dump = perf[perf.index('void dosvm_perf_dump('):]
for section, expected in ((live, 'protected'), (dump, 'verbose')):
    names = [m[0].split('(')[0].strip() for m in CALL.finditer(TOKEN.sub(
        lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), section))]
    assert names and all(PUBLICATION_CLASSES[name] == expected for name in names), names
assert PUBLICATION_CLASSES['rec_diag_disk'] == PUBLICATION_CLASSES['rec_emit']
assert LOG_CLASSES['klog_periodic'] == 'protected'
print('PASS evidence record scope: explicit probe/output ownership, .inc coverage, protected live / verbose historical classes')
