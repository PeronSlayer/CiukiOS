"""Reprice the fx-doswin3 T23 CPU banks with production replay ratios.

Usage: python3 -B tests/host/dosvm_window_cost_model.py BEFORE_LOG AFTER_LOG
Only aggregate measurements from image 5bb73dba are retained. No private
trace or game payload is required. This is a VM/presenter cost model, not
an FPS estimate; see docs/validation/fx-doswin3.md for its assumptions.
"""
import argparse
from pathlib import Path
import re


def replay_rows(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('replay='):
            row = dict(re.findall(r'(\w+)=([^ ]+)', line))
            result[row['replay']] = row
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    before, after = replay_rows(args.before), replay_rows(args.after)
    scaler = max(float(after[k]['fault_us_per_frame']) / float(before[k]['fault_us_per_frame'])
                 for k in ('wolf-near-width4', 'wolf-mid-width2'))
    palette = float(after['war2-palette']['host_us_per_load']) / float(before['war2-palette']['host_us_per_load'])
    # Each input bank lasts 5,000 ms. Nested port/remap times are already
    # included in VGA I/O; never add them to that enclosing category.
    # name, guest, monitor, VGA I/O, VGA PF, presenter, optimized CPU, ratio
    cases = [
        ('DOOM', 1038023, 1785788, 907307, 73157, 102724, 0, 1),
        ('Wolfenstein', 550177, 2315028, 646740, 885439, 176473, 885439, scaler),
        ('WarcraftII', 353965, 2385749, 1771541, 0, 231547, 1771540, palette),
    ]
    print(f'scaler_ratio={scaler:.9f} palette_ratio={palette:.9f}')
    for name, guest, monitor, io, fault, present, optimized, ratio in cases:
        reference = guest + monitor - io - fault
        old = guest + monitor + present
        new = old - optimized * (1 - ratio)
        remainder = 5000000 - old
        assert old + remainder == 5000000 and remainder >= 0
        assert optimized <= io + fault
        print(f'{name}: reference_us={reference} before_us={old} after_us={new:.0f} '
              f'before_factor={old/reference:.6f} after_factor={new/reference:.6f} '
              f'other_off_cpu_us={remainder} measured_wall_us={old+remainder}')
    print('Reference = same-trace native-video CPU work, not a measured full-screen frame.')
    print('Wolf assumes scaler-dominated PF CPU; Warcraft assumes the replayed counted-loop mix.')
    print('Desktop viewport/composition/PCI costs remain unpriced; batching gets no speculative speedup.')
    print('No wall-time/FPS/hardware qualification is implied by these ratios.')


if __name__ == '__main__':
    main()
