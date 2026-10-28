#!/usr/bin/env python3
"""Summarize one fx-selftest run (docs/design/self-test.md, "Report format").

Inputs are copies taken after the boot: C:\\SYSTEM\\LOGS\\SELFTEST.LOG from
the disk, and/or a serial capture (or BOOTLOG.TXT) holding the
`CIUKI_TEST ... probe=selftest` records. Nothing is mounted or written.

    python3 scripts/test/selftest_report.py SELFTEST.LOG [serial.txt] [--json] [--run XXXXXXXX]

Exit status: 0 complete run without failures, 1 complete run with failures,
2 incomplete, inconsistent or unreadable report.
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import json
import re
import sys
from pathlib import Path

FORMAT = 1
LIMIT = 4 * 1024 * 1024
RECORD_LIMIT = 240
CHECK_ID = re.compile(r'[a-z0-9][a-z0-9._-]{0,23}')
TOKEN = re.compile(r'([a-z][a-z0-9_]*)=([A-Za-z0-9_:.,/+\-]+)')
STATUS = {'PASS': 'pass', 'FAIL': 'fail', 'SKIP': 'skip'}
ENVELOPE = ('v', 'run', 'seq', 'probe', 'event')


class ReportError(ValueError):
    """The input is not a self-test report of this format."""


def tokens(text, where):
    """Ordered key=value tokens of the record grammar; a duplicate key is an error."""
    out = {}
    for token in text.split():
        match = TOKEN.fullmatch(token)
        if not match:
            raise ReportError(f'{where}: invalid token {token!r}')
        if match[1] in out:
            raise ReportError(f'{where}: duplicate key {match[1]!r}')
        out[match[1]] = match[2]
    return out


def empty_report(source):
    return {'source': source, 'format': None, 'run': None, 'build': None, 'os': None,
            'checks': [], 'details': [], 'summary': None, 'stalled': [], 'running': None,
            'truncated': False, 'log': None, 'complete': False, 'problems': []}


def add_check(report, check_id, status, ms, figures, where):
    if not CHECK_ID.fullmatch(check_id):
        raise ReportError(f'{where}: invalid check id {check_id!r}')
    if any(c['id'] == check_id for c in report['checks']):
        report['problems'].append(f'check {check_id} reported twice')
    if status == 'skip' and 'reason' not in figures:
        report['problems'].append(f'SKIP {check_id} has no reason')
    report['checks'].append({'id': check_id, 'status': status, 'ms': ms, 'figures': figures})


def parse_log(text, source='SELFTEST.LOG'):
    """SELFTEST.LOG: header, one line per check, the notice and SUMMARY, the
    Details section and the LOG line; RUNNING/STALLED/LOG TRUNCATED when the
    run did not finish normally."""
    report = empty_report(source)
    lines = text.replace('\r\n', '\n').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    if not lines or not lines[0].startswith('CiukiOS self-test '):
        raise ReportError(f'{source}: missing "CiukiOS self-test" header')
    header = tokens(lines[0][len('CiukiOS self-test '):], f'{source}:1')
    if header.get('format') != str(FORMAT):
        raise ReportError(f'{source}: unsupported format {header.get("format")!r}')
    if not re.fullmatch(r'[0-9a-f]{8}', header.get('run', '')):
        raise ReportError(f'{source}: invalid run id')
    report.update(format=FORMAT, run=header['run'], build=header.get('build'), os=header.get('os'))
    section = 'checks'
    for number, line in enumerate(lines[1:], 2):
        where = f'{source}:{number}'
        word, _, rest = line.partition(' ')
        if line == 'LOG TRUNCATED':
            report['truncated'] = True
        elif word == 'RUNNING':
            report['running'] = rest
        elif word == 'STALLED':
            check_id, _, figures = rest.partition(' ')
            report['stalled'].append({'id': check_id, 'ms': int(tokens(figures, where).get('ms', 0))})
        elif line.startswith('LOG writes='):
            report['log'] = {k: int(v) for k, v in tokens(line[4:], where).items()}
        elif section == 'checks' and word in STATUS:
            check_id, _, figures = rest.partition(' ')
            values = tokens(figures, where)
            if 'ms' not in values:
                raise ReportError(f'{where}: check line without ms')
            ms = int(values.pop('ms'))
            add_check(report, check_id, STATUS[word], ms, values, where)
        elif section == 'checks' and line.startswith('Self-test complete: '):
            report['notice'] = line
        elif section == 'checks' and word == 'SUMMARY':
            report['summary'] = {k: int(v) for k, v in tokens(rest, where).items()}
        elif section == 'checks' and line == '':
            continue
        elif line == 'Details:':
            section = 'details'
        elif section == 'details' and line:
            check_id, _, figures = line.partition(' ')
            report['details'].append({'id': check_id, 'figures': tokens(figures, where)})
        else:
            raise ReportError(f'{where}: unexpected line {line[:60]!r}')
    finish(report)
    return report


def record_lines(data):
    """CIUKI_TEST records of the selftest probe from a serial capture or
    BOOTLOG.TXT. UART noise before the prefix is skipped; the record itself
    must follow the controller grammar exactly."""
    for number, raw in enumerate(data.split(b'\n'), 1):
        at = raw.find(b'CIUKI_TEST v=1 ')
        if at < 0 or b' probe=selftest ' not in raw[at:]:
            continue
        line = raw[at:].rstrip(b'\r')
        if len(line) > RECORD_LIMIT:
            raise ReportError(f'line {number}: record exceeds {RECORD_LIMIT} bytes')
        try:
            text = line.decode('ascii')
        except UnicodeDecodeError:
            raise ReportError(f'line {number}: non-ASCII record') from None
        record = tokens(text[len('CIUKI_TEST '):], f'line {number}')
        if any(k not in record for k in ENVELOPE) or not re.fullmatch(r'[0-9]{6}', record['seq']):
            raise ReportError(f'line {number}: invalid record envelope')
        yield number, record


def parse_serial(data, source='serial', run=None):
    """Serial records of one run: BEGIN, DATA (check, part=N continuation,
    part=detail, status=stalled) and END. Without run, the last BEGIN wins."""
    records = list(record_lines(data))
    begins = [r for _, r in records if r['event'] == 'BEGIN']
    if not begins:
        raise ReportError(f'{source}: no selftest BEGIN record')
    run = run or begins[-1]['run']
    report = empty_report(source)
    previous = -1
    for number, r in records:
        if r['run'] != run:
            continue
        where = f'{source}:{number}'
        seq = int(r['seq'])
        if seq <= previous:
            report['problems'].append(f'{where}: sequence {seq} not increasing')
        previous = seq
        fields = {k: v for k, v in r.items() if k not in ENVELOPE}
        event = r['event']
        if event == 'BEGIN':
            if fields.get('format') != str(FORMAT):
                raise ReportError(f'{where}: unsupported format {fields.get("format")!r}')
            report.update(format=FORMAT, run=run, build=fields.get('build'), os=fields.get('os'))
            report['checks'].clear(); report['details'].clear(); report['summary'] = None
        elif event == 'END':
            report['summary'] = {k: int(v) for k, v in fields.items() if k != 'status'}
            report['end_status'] = fields.get('status')
        elif event == 'DATA':
            check_id = fields.pop('check', None)
            if check_id is None:
                raise ReportError(f'{where}: DATA record without check')
            part = fields.pop('part', None)
            status = fields.pop('status', None)
            if check_id == 'selftest.log' and part == 'detail':
                report['log'] = {k: int(v) for k, v in fields.items() if k in ('writes', 'error')}
            elif part == 'detail':
                report['details'].append({'id': check_id, 'figures': fields})
            elif part == 'detail_more':
                last = report['details'][-1] if report['details'] else None
                if not last or last['id'] != check_id or set(fields) & set(last['figures']):
                    raise ReportError(f'{where}: detail continuation without its line')
                last['figures'].update(fields)
            elif status == 'stalled':
                report['stalled'].append({'id': check_id, 'ms': int(fields.get('ms', 0))})
            elif part is not None:
                last = report['checks'][-1] if report['checks'] else None
                if not last or last['id'] != check_id:
                    raise ReportError(f'{where}: continuation without its check')
                for key, value in fields.items():
                    if key in last['figures']:
                        raise ReportError(f'{where}: duplicate key {key!r} in {check_id}')
                    last['figures'][key] = value
            elif status in STATUS.values():
                ms = int(fields.pop('ms', -1))
                add_check(report, check_id, status, ms, fields, where)
            else:
                raise ReportError(f'{where}: DATA record without status')
        elif event == 'ERROR':
            report['problems'].append(f'{where}: ERROR record {fields}')
    if report['format'] is None:
        raise ReportError(f'{source}: run {run} has no BEGIN record')
    finish(report)
    return report


def finish(report):
    """Totals from the check lines, compared with the summary when present."""
    counts = {s: sum(c['status'] == s for c in report['checks']) for s in STATUS.values()}
    report['counts'] = {'passed': counts['pass'], 'failed': counts['fail'],
                        'skipped': counts['skip'], 'checks': len(report['checks'])}
    summary = report['summary']
    if summary is not None:
        for key in ('passed', 'failed', 'skipped', 'checks'):
            if summary.get(key) != report['counts'][key]:
                report['problems'].append(f'summary {key}={summary.get(key)} but {report["counts"][key]} check lines')
        end = report.get('end_status')
        if end is not None and end != ('FAIL' if summary.get('failed') else 'PASS'):
            report['problems'].append(f'END status={end} disagrees with failed={summary.get("failed")}')
    report['complete'] = (summary is not None and not report['running'] and not report['stalled']
                          and not report['truncated'])


def compare(log, serial):
    """Differences between the disk copy and the serial records of one run."""
    out = []
    if log['run'] != serial['run']:
        return [f'run differs: log {log["run"]}, serial {serial["run"]}']
    serial_checks = {c['id']: c for c in serial['checks']}
    for check in log['checks']:
        other = serial_checks.get(check['id'])
        if other is None:
            out.append(f'{check["id"]}: missing from the serial records')
        elif (other['status'], other['ms'], other['figures']) != (check['status'], check['ms'], check['figures']):
            out.append(f'{check["id"]}: serial record differs from the log line')
    for check_id in serial_checks.keys() - {c['id'] for c in log['checks']}:
        out.append(f'{check_id}: missing from the log')
    return out


def load(path, run=None):
    data = Path(path).read_bytes()
    if len(data) > LIMIT:
        raise ReportError(f'{path}: larger than {LIMIT} bytes')
    if data.startswith(b'CiukiOS self-test '):
        try:
            return parse_log(data.decode('ascii'), str(path))
        except UnicodeDecodeError:
            raise ReportError(f'{path}: non-ASCII log') from None
    return parse_serial(data, str(path), run)


def summary_text(report):
    c = report['counts']
    lines = [f'CiukiOS self-test run {report["run"]} (build {report["build"]}, os {report["os"]}) from {report["source"]}']
    seconds = report['summary'].get('ms', 0) / 1000 if report['summary'] else None
    state = 'complete' if report['complete'] else (
        f'incomplete: stopped while running {report["running"]}' if report['running'] else
        f'incomplete: stalled in {", ".join(s["id"] for s in report["stalled"])}' if report['stalled'] else
        'incomplete: log truncated' if report['truncated'] else 'incomplete: no summary')
    took = f' in {seconds:.1f} s' if seconds is not None else ''
    lines.append(f'Result: {c["passed"]} passed, {c["failed"]} failed, {c["skipped"]} skipped '
                 f'of {c["checks"]} checks{took} ({state})')
    # Failures with every figure, skips with their reason only.
    for check in report['checks']:
        if check['status'] == 'fail':
            figures = ' '.join(f'{k}={v}' for k, v in check['figures'].items())
            lines.append(f'FAIL {check["id"]} ({check["ms"]} ms): {figures}')
    skipped = [f'{c["id"]} ({c["figures"].get("reason", "?")})' for c in report['checks'] if c['status'] == 'skip']
    if skipped:
        lines.append('SKIP ' + ', '.join(skipped))
    for stall in report['stalled']:
        lines.append(f'STALLED {stall["id"]} after {stall["ms"]} ms')
    if report['log'] is not None and report['log'].get('error'):
        lines.append(f'SELFTEST.LOG write error {report["log"]["error"]}')
    lines.extend('PROBLEM ' + p for p in report['problems'])
    return '\n'.join(lines)


def exit_status(reports, differences):
    if any(not r['complete'] or r['problems'] for r in reports) or differences:
        return 2
    return 1 if any(r['counts']['failed'] for r in reports) else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('inputs', nargs='+', help='SELFTEST.LOG and/or a serial capture or BOOTLOG.TXT')
    parser.add_argument('--run', help='run id to select from a serial capture (default: the last one)')
    parser.add_argument('--json', action='store_true', help='print the parsed reports as JSON')
    args = parser.parse_args(argv)
    if args.run and not re.fullmatch(r'[0-9a-f]{8}', args.run):
        parser.error('--run takes 8 lowercase hex digits')
    try:
        reports = [load(path, args.run) for path in args.inputs]
    except (OSError, ReportError) as e:
        print(f'selftest_report: {e}', file=sys.stderr)
        return 2
    # Two inputs are the disk copy and the serial records of the same run.
    differences = compare(reports[0], reports[1]) if len(reports) == 2 else []
    if args.json:
        print(json.dumps({'reports': reports, 'differences': differences}, indent=2))
    else:
        print('\n\n'.join(summary_text(r) for r in reports))
        for d in differences:
            print('DIFFERENCE ' + d)
    return exit_status(reports, differences)


if __name__ == '__main__':
    sys.exit(main())
