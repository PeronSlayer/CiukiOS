#!/usr/bin/env python3
"""Per-title lead entry point: plan, verify download, master media or run a case.

Defaults to an I/O-free plan. Original extraction/installation, exact input
steps, reference pins and guest drivers are supplied by the lead (f5-15B).
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'scripts/test'))
from scripts.win32 import workloads
import win32_fixtures
import win32_runner
from evidence import EvidenceNotRun


def plan(workload, tier='titles', root=ROOT):
    row = win32_fixtures.workload_row(workload, root)
    suite = 'f5-' + tier
    data = json.loads((Path(root) / f'tests/suites/{suite}.json').read_text())
    case = next(c for c in data['cases'] if c['win32']['workload'] == workload)
    command = ['python3', '-B', 'scripts/test/run.py', suite, '--case', case['id']]
    return row, case, {'workload': workload, 'tier': tier, 'case': case['id'],
        'download': str(Path('build/downloads/win32') / workload / row['source']['file']),
        'extracted': str(win32_fixtures.source_directory(row, {'source': 'extracted'}, root)),
        'installed': str(win32_fixtures.source_directory(row, {'source': 'installed'}, root)),
        'input_script': case['win32']['script'], 'operator_script': row['installer']['operator_script'],
        'command': command, 'evidence': f'build/test-runs/{suite}/<run-id>/result.json and serial.log',
        'status': ('guest-driven installers tier (f5-15C): --calibrate install, --calibrate full, review, --accept'
                   if tier == 'installers' else 'authored; original-title reference qualification and guest title driver pending')}


def _main(workload, argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tier', choices=('installers', 'titles'), default='titles')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--verify', action='store_true')
    action.add_argument('--fetch', action='store_true', help='lead only: verified manifest downloader')
    action.add_argument('--master', type=Path, metavar='RUN_DIRECTORY')
    action.add_argument('--run', action='store_true', help='lead only: invoke the canonical runner')
    action.add_argument('--calibrate', choices=('install', 'full'),
                        help='lead only, installers tier: QEMU calibration run (install first, then full)')
    action.add_argument('--accept', action='store_true',
                        help='lead only, installers tier: accept the calibrated manifest, operator program and screen pins')
    parser.add_argument('--import-unpinned', action='store_true', help='SHA-1 candidate import; never qualifies a run')
    args = parser.parse_args(argv)
    row, case, output = plan(workload, args.tier)
    if args.fetch:
        output['download_receipt'] = workloads.fetch(row, import_unpinned=args.import_unpinned)
    elif args.verify or args.master:
        # Keep archive verification separate from extraction: a matching archive
        # does not prove that the extracted/installed tree is reference-correct.
        output['verification'] = workloads.verify(ROOT / output['download'], row['source'], args.import_unpinned)
        output['verification_status'] = 'pinned' if row['source']['sha256'] else 'candidate-unreviewed'
        if args.master:
            if output['verification_status'] != 'pinned':
                raise ValueError('mastering requires a lead-reviewed SHA-256 pin')
            output['fixtures'] = [win32_fixtures.generate(f, args.master, i) for i, f in enumerate(case['fixtures'])]
    elif args.run:
        win32_runner.validate_case(case)
        win32_runner.precheck(case)  # no launch with provisional scripts/pins
        return subprocess.run(output['command'], cwd=ROOT).returncode
    elif args.calibrate:
        if args.tier != 'installers':
            parser.error('--calibrate applies to --tier installers')
        calibration = dict(case, _win32_calibrate=args.calibrate)
        win32_runner.validate_case(calibration)
        win32_runner.precheck(calibration)  # deferred (Win16) and unregistered probes stop here
        return subprocess.run([*output['command'], '--win32-calibrate', args.calibrate], cwd=ROOT).returncode
    elif args.accept:
        if args.tier != 'installers':
            parser.error('--accept applies to --tier installers')
        output['accepted'] = accept(workload, case)
    print(json.dumps(output, indent=2))
    return 0


def accept(workload, case, root=ROOT):
    """f5-15C lead acceptance after reviewing the calibration run's frames,
    serial and the workloads.json diff: the calibrated manifest becomes
    verified, the operator program qualified, the calibrated screen digests
    become the case's pins and the case's uninstall flag follows the
    registration. Writes tracked files only; the lead commits them."""
    root = Path(root)
    manifest_path = root / 'tests/win32/workloads.json'
    data = json.loads(manifest_path.read_text())
    row = next(r for r in data['workloads'] if r['id'] == workload)
    inventory = row['installed_files']
    if inventory['status'] != 'calibrated' or not (inventory.get('calibration') or {}).get('full'):
        raise ValueError('accept needs both calibration runs (--calibrate install, then full)')
    inventory['status'] = 'verified'
    inventory['reason'] = 'calibrated by the QEMU installers tier and accepted by the lead'
    workloads.validate(data)
    manifest_path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n')
    operator_path = root / 'tests/win32/operators' / (workload + '.json')
    operator = json.loads(operator_path.read_text())
    operator['guest']['status'] = 'qualified'
    operator_path.write_text(json.dumps(operator, indent=2, ensure_ascii=False) + '\n')
    suite_path = root / 'tests/suites/f5-installers.json'
    suite = json.loads(suite_path.read_text())
    target = next(c for c in suite['cases'] if c['id'] == case['id'])
    screens = {**inventory['calibration'].get('screens', {}), **inventory['calibration']['full'].get('screens', {})}
    for name, pin in screens.items():
        if name in target['win32']['screens']:
            target['win32']['screens'][name]['sha256'] = pin
    target['win32']['uninstall'] = bool((inventory.get('uninstall') or {}).get('registered'))
    target.pop('integration_pending', None)
    suite_path.write_text(json.dumps(suite, indent=2, ensure_ascii=False) + '\n')
    return {'manifest': 'verified', 'operator': 'qualified', 'screens': sorted(screens),
            'uninstall': target['win32']['uninstall']}


def main(workload, argv=None):
    try:
        return _main(workload, argv)
    except EvidenceNotRun as error:
        print(json.dumps({'status': 'not_run', 'workload': workload, 'reason': str(error),
                          'blockers': getattr(error, 'blockers', [])}, indent=2))
        return 0
