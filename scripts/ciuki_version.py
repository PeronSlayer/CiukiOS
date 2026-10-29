#!/usr/bin/env python3
"""CiukiOS identity; SemVer 2.0.0 is authoritative, stage is derived."""
# SPDX-License-Identifier: GPL-2.0-only
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
PATH = ROOT / "config/version.json"
# Official named-group regex: https://semver.org/spec/v2.0.0.html#is-there-a-suggested-regular-expression-regex-to-check-a-semver-string
SEMVER = re.compile(r"^(?P<major>0|[1-9]\d*)\.(?P<minor>0|[1-9]\d*)\.(?P<patch>0|[1-9]\d*)(?:-(?P<prerelease>(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+(?P<buildmetadata>[0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$", re.ASCII)
PRERELEASE = re.compile(r"(alpha|beta|rc)\.([1-9][0-9]*)")


def parse(value):
    """Validate generic SemVer, including metadata, for precedence."""
    match = SEMVER.fullmatch(value) if isinstance(value, str) else None
    if not match:
        raise ValueError("version must follow Semantic Versioning 2.0.0")
    return match.groupdict()


def identity(value):
    """CiukiOS release identity (metadata belongs to the build, not config)."""
    parts = parse(value)
    pre = parts['prerelease']
    match = PRERELEASE.fullmatch(pre) if pre else None
    if parts['buildmetadata'] or (pre and not match):
        raise ValueError("version must be X.Y.Z or X.Y.Z-alpha.N/beta.N/rc.N (N >= 1), without metadata")
    core = '.'.join(parts[k] for k in ('major', 'minor', 'patch'))
    stage, number = (match[1], int(match[2])) if match else ('stable' if int(parts['major']) else 'development', None)
    display = core + (f" {dict(alpha='Alpha', beta='Beta', rc='Release Candidate')[stage]} {number}" if pre else '')
    return {'version': value, 'core': core, 'number': number, 'stage': stage,
            'prerelease_number': number, 'display': display, 'human_name': 'CiukiOS ' + display}


def with_build(value, build_id, commit):
    """Append metadata without changing precedence; allow the dirty build-id suffix."""
    identity(value)
    if not re.fullmatch(r'[0-9A-Za-z-]+', build_id) or not re.fullmatch(r'[0-9a-f]+|unknown', commit):
        raise ValueError('invalid build identity')
    return f'{value}+build.{build_id}.g{commit}'


def compare(left, right):
    """Return -1, 0 or 1 using SemVer section 11 (metadata ignored)."""
    a, b = parse(left), parse(right)
    for key in ('major', 'minor', 'patch'):
        x, y = int(a[key]), int(b[key])
        if x != y:
            return (x > y) - (x < y)
    x, y = a['prerelease'], b['prerelease']
    if x is None or y is None:
        return (x is None) - (y is None)
    xs, ys = x.split('.'), y.split('.')
    for x, y in zip(xs, ys):
        if x == y:
            continue
        if x.isdigit() and y.isdigit():
            return (int(x) > int(y)) - (int(x) < int(y))
        if x.isdigit() != y.isdigit():
            return -1 if x.isdigit() else 1
        return (x > y) - (x < y)
    return (len(xs) > len(ys)) - (len(xs) < len(ys))


def load(path=PATH):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    try:
        if 'stage' in data:
            raise ValueError('stage must be derived from version')
        return identity(data.get('version'))
    except ValueError as error:
        raise SystemExit(f'{path}: {error}') from error


if __name__ == '__main__':
    print(load()['human_name'])
