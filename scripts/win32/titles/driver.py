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
        'status': 'authored; original-title reference qualification and guest driver pending'}


def _main(workload, argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tier', choices=('installers', 'titles'), default='titles')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--verify', action='store_true')
    action.add_argument('--fetch', action='store_true', help='lead only: verified manifest downloader')
    action.add_argument('--master', type=Path, metavar='RUN_DIRECTORY')
    action.add_argument('--run', action='store_true', help='lead only: invoke the canonical runner')
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
    print(json.dumps(output, indent=2))
    return 0


def main(workload, argv=None):
    try:
        return _main(workload, argv)
    except EvidenceNotRun as error:
        print(json.dumps({'status': 'not_run', 'workload': workload, 'reason': str(error),
                          'blockers': getattr(error, 'blockers', [])}, indent=2))
        return 0
