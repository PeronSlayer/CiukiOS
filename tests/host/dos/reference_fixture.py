"""Locate provisioned release media for optional host regression evidence."""
import json
import conformance


def provisioned_reference(test):
    canonical = conformance.ROOT / 'third_party/freedos/1.3'
    if not (canonical / 'UPSTREAM.json').is_file():
        test.skipTest('FreeDOS 1.3 pinned reference unavailable; missing pin file: ' + str(canonical / 'UPSTREAM.json'))
    pin = json.loads((canonical / 'UPSTREAM.json').read_text())
    needed = sorted({row['archive'] for row in pin.values() if isinstance(row, dict) and 'archive' in row} | {'boot16.bin'})
    absent = [name for name in needed if not (canonical / name).is_file()]
    if absent:
        test.skipTest('FreeDOS 1.3 pinned reference unavailable; missing files: ' + str(canonical) + ': ' + ', '.join(absent))
    return canonical
