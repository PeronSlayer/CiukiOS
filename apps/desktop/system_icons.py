#!/usr/bin/env python3
"""Tango-first system icon preparation; no network or third-party renderer.

Only the selected Haiku exports supplement Tango. The original Tango store
and its pixel oracle remain independent. Sources stay in pinned archives.
"""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
import tarfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'assets/icons'
RECORD = ASSETS / 'system-store.json'
INVENTORY = ASSETS / 'inventory.json'
HAIKU = ASSETS / 'upstream/haiku-icons.tar.gz'
HAIKU_SHA = 'e956e60f84c12bd1d946a68c4df6f49c49ab7329365390c434e6dbbdb9a560b5'
SELECTED = {
    'haiku-clock': 'App_Clock',
    'haiku-log-viewer': 'Server_Syslog',
    'haiku-serial': 'App_SerialConnect',
    'haiku-registry': 'App_ResourceEdit',
    'haiku-system-monitor': 'App_Pulse',
    'haiku-resource-meter': 'App_Chart',
    'haiku-system-info': 'Prefs_Devices',
    'haiku-mines': 'Misc_Bomb',
}
# Scope of the static icon catalogue: map, shell tables, CP hierarchy, Files,
# device classes and common dialogs. Arbitrary user shortcuts use documented
# type/program fallbacks; external application artwork is not system artwork.
CATALOGUES = (
    'apps/libdeskui/icon_map.c', 'apps/desktop/shell.c',
    'apps/desktop/catalog.h', 'apps/desktop/settings_catalog.h',
    'apps/control/app.c', 'apps/control/hardware.inc', 'apps/files/app.c',
    'apps/libdeskui/dialog.c', 'apps/libdeskui/commdlg.c',
    'apps/wordpad/app.c', 'apps/systools/sysinfo_view.c', 'apps/paint/paint.c',
    'apps/systools/schedtask_ui.c', 'apps/tasks/view.c', 'apps/regedit/app.c',
    'apps/tasks/tasks.c', 'apps/ciukide/app.c', 'apps/accessories/viewer_view.c',
    'apps/accessories/scandisk_view.c', 'apps/desktop/desktop.c',
    'apps/desktop/logon.inc', 'apps/desktop/taskbar_custom.inc',
)
PREVIOUS = {
    'program:clock': 'appointment-new', 'program:logview': 'x-office-spreadsheet',
    'program:serial': 'network-wired', 'program:regedit': 'x-office-address-book',
    'program:sysmon': 'network-transmit-receive', 'program:resmeter': 'applications-system',
    'program:sysinfo': 'document-properties', 'program:mines': 'face-smile',
    'program:freecell': 'face-glasses',
}
NOTICES = {'TANGO.TXT': 'upstream/COPYING', 'AUTHORS.TXT': 'upstream/AUTHORS',
           'CREDITS.TXT': 'CREDITS.TXT', 'HAIKU.TXT': 'upstream/HAIKU-LICENSE',
           'HAIKU-AUTHORS.TXT': 'upstream/HAIKU-README.md'}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def converter():
    return load('system_tango', ROOT / 'apps/desktop/convert_icons.py')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sources():
    c = converter()
    icons = c.store_sources()
    if sha(HAIKU.read_bytes()) != HAIKU_SHA:
        raise ValueError('Haiku archive changed')
    with tarfile.open(HAIKU) as archive:
        for notice, member in [('HAIKU-LICENSE', 'LICENSE'), ('HAIKU-README.md', 'README.md')]:
            if (ASSETS / 'upstream' / notice).read_bytes() != archive.extractfile('haiku-icons-master/' + member).read():
                raise ValueError('Haiku notice changed')
        for name, original in SELECTED.items():
            png = {}
            for size in (16, 32, 64):
                member = f'haiku-icons-master/png/{size}x{size}/{original}.png'
                raw = archive.extractfile(member).read()
                w, h, rgba = c.png_rgba(raw)
                if (w, h) != (size, size):
                    raise ValueError('Haiku PNG geometry')
                # Precompute missing store sizes from 64, never upscale 32 to 48.
                # CIUKICS1 permits 16/22/32/48 source images only.
                if size == 64:
                    for dst in (22, 48):
                        png[dst] = (member, sha(raw), c.area_scale(c.canonical(rgba), size, dst))
                else:
                    png[size] = (member, sha(raw), c.canonical(rgba))
            icons[name] = {'context': 'apps', 'png': png}
    return icons


def build_store():
    c = converter()
    notice = c.NOTICE + ' Haiku icons: Copyright (c) 2007-2026 Haiku, Inc.; MIT. See HAIKU.TXT and HAIKU-AUTHORS.TXT. Archive SHA-256 ' + HAIKU_SHA + '.'
    data, record = c.build_store(sources=sources(), notice_text=notice)
    record['license'] = 'Per-icon Public Domain / MIT; see MANIFEST.JSON and shipped notices'
    record['conversion'] += '; Haiku native 16/32, 22/48 area-filtered from 64'
    record['supplements'] = {'Haiku': {'archive_sha256': HAIKU_SHA, 'license': 'MIT', 'icons': SELECTED}}
    return data, record


def store_record():
    return json.loads(RECORD.read_text())


def check_store(data):
    record = store_record()
    if sha(data) != record['store_sha256'] or len(data) != record['store_bytes']:
        raise ValueError('System icon store differs from system-store.json')
    return record


def check_store_record():
    data, record = build_store()
    if record != store_record():
        raise ValueError('System icon provenance changed')
    check_store(data)
    return record


def inventory(icons):
    rows = []
    text = (ROOT / CATALOGUES[0]).read_text()
    mappings = {}
    for obj, name, identity in re.findall(r'\{"([^"\n]+)",\s*(?:"([^"\n]+)"|(DU_ICON_CIUKI))\}', text):
        mappings[obj] = 'ciuki' if identity else name
    for obj, name in mappings.items():
        old = PREVIOUS.get(obj, name)
        rows.append({'object': obj, 'icon': name, 'previous': old,
                     'status': 'replaced stand-in' if obj in PREVIOUS else 'generic fallback' if obj.endswith(':*') or obj == 'type:unknown' else 'retained',
                     'source': CATALOGUES[0]})
    # Explicit names cover catalogue tables and direct draws as well as map keys.
    known = set(icons) | {'ciuki', 'ciuki-computer'}
    for path in CATALOGUES[1:]:
        for name in sorted(set(re.findall(r'"([a-z][a-z0-9-]+)"', (ROOT / path).read_text())) & known):
            rows.append({'object': path + ':' + name, 'icon': name,
                         'previous': name, 'status': 'retained', 'source': path})
    applets = (ROOT / 'apps/desktop/settings_catalog.h').read_text()
    for name, label, icon in re.findall(r'\{CP_\w+, CP_CAT_\w+, "([^"]+)", "([^"]+)", "([^"]+)"\}', applets):
        rows.append({'object': 'applet:' + name, 'label': label, 'icon': icon,
                     'previous': icon, 'status': 'retained', 'source': 'apps/desktop/settings_catalog.h'})
    rows.append({'object': 'animation:busy', 'icon': 'process-working-*', 'previous': 'process-working-*',
                 'status': 'retained', 'source': 'apps/libdeskui/icons.c'})
    for row in rows:
        original = row['previous']
        row['previous_missing_runtime_sizes'] = []
        row['previous_derived_sizes'] = [s for s in (16, 32, 48)
                                        if original in icons and s not in icons[original]['png']]
    return sorted(rows, key=lambda row: row['object'])


def png(size, rgba):
    def chunk(kind, raw):
        return struct.pack('>I', len(raw)) + kind + raw + struct.pack('>I', zlib.crc32(kind + raw))
    pixels = b''.join(b'\0' + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(pixels, 9)) + chunk(b'IEND', b'')


def portrait_pixels(c):
    portrait = load('system_portrait', ROOT / 'apps/desktop/convert_portrait.py')
    source = ROOT / 'assets/brand/ciuki-logo.png'
    if sha(source.read_bytes()) != portrait.SOURCE_SHA256:
        raise ValueError('Approved portrait changed')
    xrgb = portrait.convert(source)
    if sha(xrgb) != portrait.ARRAY_SHA256:
        raise ValueError('Approved portrait conversion changed')
    return portrait, b''.join(bytes((xrgb[i + 2], xrgb[i + 1], xrgb[i], 255)) for i in range(0, len(xrgb), 4))


def computer_pixels(rgba, background, size):
    """Match the existing shell portrait_badge, without drawing new artwork."""
    out = bytearray(background)
    badge = size * 5 // 8
    offset = size - badge
    for y in range(badge):
        for x in range(badge):
            samples = [rgba[(sy * 256 + sx) * 4:(sy * 256 + sx) * 4 + 3]
                       for sy in range(y * 256 // badge, (y + 1) * 256 // badge)
                       for sx in range(x * 256 // badge, (x + 1) * 256 // badge)]
            visible = [p for p in samples if p != bytes((0x37, 0x55, 0x64))]
            if not visible:
                continue
            a = len(visible) * 255 // len(samples)
            pos = ((offset + y) * size + offset + x) * 4
            ba = out[pos + 3]
            oa = a + ba * (255 - a) // 255
            for ch in range(3):
                colour = sum(p[ch] for p in visible) // len(visible)
                out[pos + ch] = (colour * a + out[pos + ch] * ba * (255 - a) // 255) // oa if oa else 0
            out[pos + 3] = oa
    return bytes(out)


def payloads():
    c = converter()
    icons = sources()
    rows = inventory(icons)
    if rows != json.loads(INVENTORY.read_text()):
        raise ValueError('Icon inventory stale; run system_icons.py --prepare')
    data, record = build_store()
    check_store(data)
    parsed = c.parse_store(data)
    portrait, rgba = portrait_pixels(c)
    used = {r['icon'] for r in rows} - {'process-working-*'}
    used.update(f'process-working-{i:02}' for i in range(32))
    used.add('ciuki')
    files, manifest = {'tango.cis': data}, []
    for name in sorted(set(icons) | {'ciuki', 'ciuki-computer'}):
        identity = name in ('ciuki', 'ciuki-computer')
        haiku = name in SELECTED
        entry = {'name': name, 'set': 'Ciuki' if identity else 'Haiku' if haiku else 'Tango',
                 'license': 'Project asset (no new license granted)' if identity else 'MIT' if haiku else 'Public Domain',
                 'sources': ['assets/brand/ciuki-logo.png'] if identity else sorted({v[0] for v in icons[name]['png'].values()}),
                 'png': {}}
        if not identity:
            entry['archive'] = 'upstream/haiku-icons.tar.gz' if haiku else 'upstream/tango-icon-theme-0.8.90.tar.gz'
            entry['source_sha256s'] = {v[0]: v[1] for v in icons[name]['png'].values()}
        if identity:
            if name == 'ciuki-computer':
                entry['sources'] += sorted({v[0] for v in icons['computer']['png'].values()})
            entry['source_sha256'] = portrait.SOURCE_SHA256
            entry['conversion_sha256'] = portrait.ARRAY_SHA256
        if name in used:
            for size in (16, 32, 48):
                path = f'{name}-{size}.png'
                pixels = c.area_scale(rgba, 256, size) if identity else parsed[name][size]
                if name == 'ciuki-computer':
                    pixels = computer_pixels(rgba, parsed['computer'][size], size)
                files[path] = png(size, pixels)
                entry['png'][str(size)] = {'path': path, 'sha256': sha(files[path])}
        manifest.append(entry)
    files['MANIFEST.JSON'] = (json.dumps({'icons': manifest, 'runtime_composites': {'ciuki-computer': ['computer', 'ciuki']},
        'store_sha256': record['store_sha256'], 'sizes': [16, 32, 48]}, indent=2, sort_keys=True) + '\n').encode()
    files['INVENTORY.JSON'] = INVENTORY.read_bytes()
    return files


def output_names():
    rows = json.loads(INVENTORY.read_text())
    names = {r['icon'] for r in rows} - {'process-working-*'}
    names.update(f'process-working-{i:02}' for i in range(32))
    return ('MANIFEST.JSON', 'INVENTORY.JSON', *(f'{n}-{s}.png' for n in sorted(names) for s in (16, 32, 48)))


def check():
    converter().check()
    return check_store_record()


if __name__ == '__main__':
    if sys.argv[1:] == ['--prepare']:
        data, record = build_store()
        RECORD.write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')
        INVENTORY.write_text(json.dumps(inventory(sources()), indent=2, sort_keys=True) + '\n')
    elif len(sys.argv) == 3 and sys.argv[1] == '--stage':
        dest = Path(sys.argv[2]); dest.mkdir(parents=True, exist_ok=True)
        for name, data in payloads().items():
            (dest / name).write_bytes(data)
        for name, source in NOTICES.items():
            (dest / name).write_bytes((ASSETS / source).read_bytes())
    else:
        raise SystemExit('usage: system_icons.py --prepare | --stage DIRECTORY')
