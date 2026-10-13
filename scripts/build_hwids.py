#!/usr/bin/env python3
"""Extract named PCI and USB hardware IDs from Windows INF collections.

Only display labels are retained. INF files and driver binaries are inputs and
are never copied or modified.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MAX_JSON = 400 * 1024
ID_RE = re.compile(r'(?i)\b(PCI)\\VEN_([0-9a-f]{4})&DEV_([0-9a-f]{4})(?:&SUBSYS_([0-9a-f]{8}))?|\b(USB)\\VID_([0-9a-f]{4})&PID_([0-9a-f]{4})')
TOKEN_RE = re.compile(r'%([^%]+)%')


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
    return value[:64].rstrip(' .')


def parse_inf(text):
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
    found = []
    for entries in sections.values():
        for line in entries:
            # Models rows normally put %description% before '=' and the hardware
            # ID after it. Also accept literal IDs elsewhere in a section.
            lhs, _, rhs = line.partition('=')
            desc_match = TOKEN_RE.search(lhs)
            raw_desc = strings.get(desc_match.group(1).casefold(), '') if desc_match else ''
            desc = ascii_name(raw_desc)
            for match in ID_RE.finditer(line):
                if match.group(1):
                    kind, vendor, device = 'pci', match.group(2), match.group(3)
                else:
                    kind, vendor, device = 'usb', match.group(6), match.group(7)
                if desc:
                    found.append((kind, f'{vendor.lower()}:{device.lower()}', desc))
    return found


def collect(root):
    pci, usb = {}, {}
    infs = sorted((p for p in root.rglob('*') if p.is_file() and p.suffix.casefold() == '.inf'),
                  key=lambda p: p.as_posix().casefold())
    for path in infs:
        try:
            records = parse_inf(decode_inf(path))
        except (OSError, UnicodeError):
            continue
        for kind, key, name in records:
            dest = pci if kind == 'pci' else usb
            old = dest.get(key)
            if old is None or (len(name), name.casefold(), name) < (len(old), old.casefold(), old):
                dest[key] = name
    return infs, dict(sorted(pci.items())), dict(sorted(usb.items()))


def encoded(data):
    return (json.dumps(data, ensure_ascii=True, sort_keys=True, indent=2) + '\n').encode('ascii')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inf_dir', nargs='?', type=Path, default=os.environ.get('CIUKIOS_INF_DIR'))
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'assets/hwids')
    args = parser.parse_args(argv)
    if not args.inf_dir:
        parser.error('provide INF_DIR or set CIUKIOS_INF_DIR')
    root = Path(args.inf_dir)
    if not root.is_dir():
        parser.error(f'not an INF collection directory: {root}')
    infs, pci, usb = collect(root)
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
