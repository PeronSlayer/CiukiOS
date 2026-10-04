#!/usr/bin/env python3
"""Decode one bounded CVPACE01 timing snapshot; timings are not monitor FPS."""
import argparse
import json
from pathlib import Path
import statistics
import struct


def describe(values):
    return {
        'count': len(values),
        'median_ms': statistics.median(values) if values else None,
        'p95_ms': statistics.quantiles(values, n=100, method='inclusive')[94] if len(values) > 1 else None,
        'max_ms': max(values) if values else None,
        'over_42ms': sum(value > 42 for value in values),
    }


def analyze(path, last_seconds=None):
    data = path.read_bytes()
    magic, size, head, khz = struct.unpack_from('<8sIII', data)
    assert magic == b'CVPACE01' and size == len(data) == 32788 and khz > 0
    events = []
    for index in range(max(0, head - 2048), head):
        low, high, event, value = struct.unpack_from('<IIII', data, 20 + (index & 2047) * 16)
        events.append(((high << 32) | low, event, value))
    if last_seconds is not None and events:
        cutoff = events[-1][0] - khz * last_seconds * 1000
        events = [event for event in events if event[0] >= cutoff]
    result = {'tsc_khz': khz, 'head': head, 'retained_events': len(events),
              'duration_seconds': (events[-1][0] - events[0][0]) / khz / 1000 if events else 0}
    for event, name in ((1, 'capture_start'), (3, 'gpu_submit'), (4, 'gpu_completion')):
        times = [timestamp for timestamp, kind, _ in events if kind == event]
        result[name] = describe([(b - a) / khz for a, b in zip(times, times[1:])])
    capture = []
    last = None
    for timestamp, event, _ in events:
        if event == 1:
            last = timestamp
        elif event == 2 and last is not None:
            capture.append((timestamp - last) / khz)
            last = None
    result['capture_duration'] = describe(capture)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--last-seconds', type=float, help='restrict to the final steady-game interval')
    args = parser.parse_args()
    print(json.dumps(analyze(args.snapshot, args.last_seconds), indent=2))
