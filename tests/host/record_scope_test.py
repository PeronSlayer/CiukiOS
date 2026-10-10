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
    'probes/f2_probes_process.c',
    'probes/f2_probes_signals.c',
    'probes/f2_probes_desktop.c',
    'probes/f2_probes_files.c',
    'drivers/ata_probe.c',
    'drivers/fbdev_probe.c',
    'drivers/i8042_probe.c',
}
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
CALL = re.compile(r'\brec_emit(?:_panicsafe)?\s*\(')


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
root = Path(__file__).resolve().parents[2] / 'src/kernel'
violations = []
for path in sorted(root.rglob('*')):
    if path.suffix not in {'.c', '.h'} or path.relative_to(root).as_posix() in ALLOWED:
        continue
    violations.extend(f'{path.relative_to(root)}:{line}' for line in calls(path.read_text()))
if violations:
    print('FAIL evidence record scope: ' + ', '.join(violations), file=sys.stderr)
    sys.exit(1)
print('PASS evidence record scope: explicit probe/output ownership')
