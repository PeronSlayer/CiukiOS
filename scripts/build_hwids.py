#!/usr/bin/env python3
"""Extract named PCI and USB hardware IDs from Windows INF collections.

Only display labels are retained. INF files and driver binaries are inputs and
are never copied or modified.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MAX_JSON = 400 * 1024
ID_RE = re.compile(r'(?i)\b(PCI)\\VEN_([0-9a-f]{4})&DEV_([0-9a-f]{4})(?![0-9a-z])(?:&SUBSYS_([0-9a-f]{8}))?|\b(USB)\\VID_([0-9a-f]{4})&PID_([0-9a-f]{4})(?![0-9a-z])')
TOKEN_RE = re.compile(r'%([^%]+)%')

# Original version headers and hashes of the hwdata-distributed upstream
# snapshots. Inputs live in ignored build/downloads, never in the image.
SNAPSHOTS = {
    'pci': ('2026.10.01', '4f020f031d1f2d011b2a656a191519a056cb9744229551aed2cf7f90dfc2e18a'),
    'usb': ('2026.06.26', '34b2b2289508a866cf38b96e54b37ac52c0ab7ce8361a56116af24e3fd094421'),
}
CLASSES = {'display', 'multimedia', 'network', 'storage', 'bridges',
           'serial bus/usb', 'input', 'communication'}
INF_CLASSES = {
    'display': 'display', 'media': 'multimedia', 'image': 'multimedia',
    'net': 'network', 'hdc': 'storage', 'scsiadapter': 'storage', 'mtd': 'storage',
    'usb': 'serial bus/usb', 'pcmcia': 'bridges',
    'keyboard': 'input', 'mouse': 'input', 'hidclass': 'input',
    'biometric': 'input', 'smartcardreader': 'input',
    'ports': 'communication', 'modem': 'communication', 'wdm_modem': 'communication',
    'bluetooth': 'communication', 'btw': 'communication',
}
# System is a mixed Windows setup class. These explicit function descriptions
# identify bridges/serial-bus controllers; memory, timers, APICs etc stay out.
SYSTEM_FUNCTION_RE = re.compile(
    r'(?i)\b(?:bridge|SMBus|USB|LPC|Low Pin Count|'
    r'(?:PCI|PCIE|PCI-Express|PCI Express) Root Port|'
    r'(?:CPU|Processor) to (?:I/O|AGP|PCI)|Accelerated Graphics Port)\b')


def fields(value):
    return next(csv.reader([value], skipinitialspace=True))


def decode_inf(path):
    data = path.read_bytes()
    if data.startswith((b'\xff\xfe', b'\xfe\xff')):
        return data.decode('utf-16', errors='replace')
    if b'\0' in data:
        even_nuls = data[0::2].count(0)
        odd_nuls = data[1::2].count(0)
        if max(even_nuls, odd_nuls) > len(data) // 20:
            return data.decode('utf-16-be' if even_nuls > odd_nuls else 'utf-16-le', errors='replace')
    for encoding in ('utf-8-sig', 'cp1252'):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            pass
    return data.decode('latin-1', errors='replace')


def strip_comment(line):
    quoted = False
    for i, char in enumerate(line):
        if char == '"':
            quoted = not quoted
        elif char == ';' and not quoted:
            return line[:i]
    return line


def ascii_name(value):
    value = value.strip().strip('"').replace('""', '"')
    value = value.encode('ascii', errors='ignore').decode('ascii')
    value = re.sub(r'\s+', ' ', value)
    value = re.sub(r'[^A-Za-z0-9 .,()_+&/#:\-]', '', value).strip(' .')
    value = re.sub(r'\s+', ' ', value)
    return value[:64].rstrip(' .')


def assignment(line):
    quoted = False
    for i, char in enumerate(line):
        if char == '"':
            quoted = not quoted
        elif char == '=' and not quoted:
            return line[:i].strip(), line[i+1:].strip()
    return line, ''


def parse_inf(text, scoped=False, allowed=None):
    sections = {}
    current = ''
    for raw in text.replace('\\\r\n', '').replace('\\\n', '').splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith('[') and ']' in line:
            current = line[1:line.index(']')].strip().casefold()
            sections.setdefault(current, [])
        elif current:
            sections[current].append(line)
    strings = {}
    # Prefer neutral strings, then a stable first locale for keys absent there.
    for section in sorted((s for s in sections if s == 'strings' or s.startswith('strings.')),
                          key=lambda s: (s != 'strings', s)):
        for line in sections[section]:
            if '=' not in line:
                continue
            key, value = line.split('=', 1)
            key = key.strip().casefold()
            if key and key not in strings:
                strings[key] = value.strip().strip('"')
    def resolve(value):
        value = TOKEN_RE.sub(lambda m: strings.get(m[1].casefold(), m[0]), value)
        return '' if TOKEN_RE.search(value) else value.strip().strip('"')

    bases = set()
    for line in sections.get('manufacturer', []):
        lhs, rhs = assignment(line)
        base = resolve(fields(rhs)[0] if rhs else lhs).casefold()
        if base:
            bases.add(base)
    # Minimal synthetic/old inputs without Manufacturer may use [Models].
    if not bases and 'manufacturer' not in sections:
        bases.add('models')
    model_sections = sorted(s for s in sections if any(
        s == base or s.startswith(base + '.') for base in bases))
    setup_class = ''
    for line in sections.get('version', []):
        lhs, rhs = assignment(line)
        if lhs.casefold() == 'class':
            setup_class = resolve(rhs).casefold()
    found = []
    for section in model_sections:
        entries = sections[section]
        for line in entries:
            lhs, rhs = assignment(line)
            desc = ascii_name(resolve(lhs)) if rhs else ''
            in_class = setup_class in INF_CLASSES or (
                setup_class == 'system' and SYSTEM_FUNCTION_RE.search(desc))
            # Require install section, then IDs; never extract registry values.
            model = fields(rhs)
            if len(model) < 2 or not model[0].strip():
                continue
            for match in ID_RE.finditer(','.join(model[1:])):
                if match.group(1):
                    kind, vendor, device = 'pci', match.group(2), match.group(3)
                else:
                    kind, vendor, device = 'usb', match.group(6), match.group(7)
                key = f'{vendor.lower()}:{device.lower()}'
                if desc and (not scoped or in_class or key in (allowed or {}).get(kind, set())):
                    found.append((kind, key, desc))
    return found


def collect(root, scoped=False, allowed=None):
    pci, usb = {}, {}
    infs = sorted((p for p in root.rglob('*') if p.is_file() and p.suffix.casefold() == '.inf'),
                  key=lambda p: p.as_posix().casefold())
    for path in infs:
        try:
            records = parse_inf(decode_inf(path), scoped=scoped, allowed=allowed)
        except (OSError, UnicodeError):
            continue
        for kind, key, name in records:
            dest = pci if kind == 'pci' else usb
            old = dest.get(key)
            if old is None or (len(name), name.casefold(), name) < (len(old), old.casefold(), old):
                dest[key] = name
    return infs, dict(sorted(pci.items())), dict(sorted(usb.items()))


def parse_ids(text):
    """Read vendor/device rows only; class/subsystem/interface rows are separate."""
    vendor = None
    result = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        top = re.fullmatch(r'([0-9a-fA-F]{4})\s+(.+)', line)
        device = re.fullmatch(r'\t([0-9a-fA-F]{4})\s+(.+)', line)
        if top:
            vendor = (top[1].lower(), top[2])
        elif device and vendor:
            name = ascii_name(vendor[1] + ' ' + device[2])
            if name:
                result.setdefault(vendor[0] + ':' + device[1].lower(), name)
        elif not line.startswith('\t'):
            vendor = None
    return result


def read_snapshot(kind, path):
    data = path.read_bytes()
    version, digest = SNAPSHOTS[kind]
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f'{kind}.ids SHA-256 mismatch (expected {version}: {digest})')
    return parse_ids(data.decode('utf-8'))


def database_names(pci, usb, pci_ids, usb_ids, scope):
    """Supplement only individually classified pairs; INF labels always win."""
    result = {'pci': dict(pci), 'usb': dict(usb)}
    for kind, source in (('pci', pci_ids), ('usb', usb_ids)):
        for category, keys in scope[kind].items():
            if category not in CLASSES:
                raise ValueError(f'unsupported hardware class: {category}')
            for key in keys:
                if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{4}', key):
                    raise ValueError(f'invalid scoped hardware ID: {key}')
                if key in source:
                    result[kind].setdefault(key, source[key])
    return dict(sorted(result['pci'].items())), dict(sorted(result['usb'].items()))


def encoded(data):
    return (json.dumps(data, ensure_ascii=True, sort_keys=True, indent=2) + '\n').encode('ascii')


def native_names(pci):
    """Fill missing labels from manufacturer manuals and upstream device tables.

    Owner-derived INF labels keep precedence; no Windows payload is added.
    """
    result = dict(pci)
    extra = json.loads((ROOT / 'assets/hwids/native-pci-ids.json').read_text())
    for key, name in extra.items():
        if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{4}', key) or ascii_name(name) != name:
            raise ValueError(f'invalid native hardware label: {key}')
        result.setdefault(key, name)
    return dict(sorted(result.items()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inf_dir', nargs='?', type=Path, default=os.environ.get('CIUKIOS_INF_DIR'))
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'assets/hwids')
    parser.add_argument('--pci-ids', type=Path, default=ROOT / 'build/downloads/pci.ids')
    parser.add_argument('--usb-ids', type=Path, default=ROOT / 'build/downloads/usb.ids')
    args = parser.parse_args(argv)
    if not args.inf_dir:
        parser.error('provide INF_DIR or set CIUKIOS_INF_DIR')
    root = Path(args.inf_dir)
    if not root.is_dir():
        parser.error(f'not an INF collection directory: {root}')
    try:
        pci_ids = read_snapshot('pci', args.pci_ids)
        usb_ids = read_snapshot('usb', args.usb_ids)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 1
    scope = json.loads((ROOT / 'assets/hwids/retro-scope.json').read_text())
    allowed = {kind: {key for keys in scope[kind].values() for key in keys}
               for kind in ('pci', 'usb')}
    infs, pci, usb = collect(root, scoped=True, allowed=allowed)
    pci = native_names(pci)
    pci, usb = database_names(pci, usb, pci_ids, usb_ids, scope)
    pbytes, ubytes = encoded(pci), encoded(usb)
    if len(pbytes) + len(ubytes) > MAX_JSON:
        print(f'error: combined JSON size {len(pbytes) + len(ubytes)} exceeds {MAX_JSON}', file=sys.stderr)
        return 1
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / 'pci-ids.json').write_bytes(pbytes)
    (args.output_dir / 'usb-ids.json').write_bytes(ubytes)
    print(f'INF files: {len(infs)}; PCI IDs: {len(pci)} ({len(pbytes)} bytes); USB IDs: {len(usb)} ({len(ubytes)} bytes); combined: {len(pbytes)+len(ubytes)} bytes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
