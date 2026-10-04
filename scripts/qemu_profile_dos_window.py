#!/usr/bin/env python3
"""Sample CS:EIP while a DOS game runs in a desktop window (diagnostic).

Attributes each `info registers` sample to SHELL.COM symbols (NASM listing),
ring 0 (Jemm/CVSESSION/HDPMI) or the DOS VM, to show where the desktop's
time goes. Host-side sampling only; the guest is not modified.
"""
import argparse, bisect, collections, json, re, shutil, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_native_windows import WindowVM


def symbols(listing):
    labels, offset = [], None
    for line in listing.read_text(errors='replace').splitlines():
        m = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', line)
        if m:
            offset = int(m.group(1), 16)
        m2 = re.search(r'^\s*\d+\s+(?:[0-9A-F]{8}\s+\S*\s+)?([A-Za-z_][\w]*):', line)
        if m2 and offset is not None and not m2.group(1).startswith('.'):
            labels.append((offset + 0x100, m2.group(1)))
    labels.sort()
    return labels


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--listing', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--samples', type=int, default=400)
    p.add_argument('--game', default='\\APPS\\DOOMVAN\\PCDMCORE.EXE -warp 1 1 -nomusic')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    disk = a.output / 'disk.img'
    shutil.copyfile(a.image, disk)
    labels = symbols(a.listing)
    keys_ = [o for o, _ in labels]
    vm = WindowVM(disk, a.output, 'std', memory=128, palette='platinum', boot_capture=True)
    counts = collections.Counter()
    ring0 = []
    v86_raw = []
    try:
        vm.ready()
        before = vm.offset()
        vm.hmp('sendkey meta_l-r 180'); time.sleep(1)
        vm.wait('WINDOW 01 OPEN', before, 20)
        for ch in a.game:
            key = {'\\': 'backslash', '.': 'dot', ' ': 'spc', '-': 'minus'}.get(
                ch, 'shift-' + ch.lower() if ch.isupper() else ch)
            vm.hmp(f'sendkey {key} 90'); time.sleep(.14)
        vm.hmp('sendkey ret 180')
        vm.wait('ST_Init: Init status bar.', before, 120)
        time.sleep(6)
        vm.hmp('mouse_move 200 300 0'); time.sleep(.5)   # focus stays on the game window
        shell_cs = None
        for sample in range(a.samples):
            if sample % 40 == 0:
                vm.hmp('sendkey up 1500')   # keep Doom's view changing
            text = vm.hmp('info registers').decode('utf-8', 'replace')
            eip = int(re.search(r'EIP=([0-9a-f]{8})', text).group(1), 16)
            cpl = int(re.search(r'CPL=(\d)', text).group(1))
            cs = re.search(r'CS =([0-9a-f]{4}) ([0-9a-f]{8})', text)
            v86 = 'VM' in re.search(r'EFL=\S+ \[([^\]]*)\]', text).group(1) or (cpl == 3 and int(cs.group(2), 16) == int(cs.group(1), 16) * 16)
            if cpl == 0:
                counts['ring0 (Jemm / CVSESSION / HDPMI)'] += 1
                linear = int(cs.group(2), 16) + eip
                counts[f'  r0 {cs.group(1)} {linear & 0xFFFFF000:08x}'] += 1
                ring0.append(linear)
            elif v86 and shell_cs is None and 0x100 <= eip < 0x10000:
                counts[f'v86 {cs.group(1)}'] += 1
            else:
                counts[f'v86 {cs.group(1)}' if v86 else 'protected-mode client'] += 1
            if v86:
                v86_raw.append((cs.group(1), eip))
                i = bisect.bisect_right(keys_, eip) - 1
                if i >= 0:
                    counts[f'  {cs.group(1)}:{labels[i][1]}'] += 1
            time.sleep(.01)
        # Ring-0 module bases: CVSESS.DLL's PE image in memory ('CVSESSION'
        # marker strings are module data); dump the low 16 MiB once.
        pages = {}
        for page in range(0xff800000, 0xff810000, 0x1000):
            text = vm.hmp(f'gva2gpa {page:#x}').decode('utf-8', 'replace')
            m = re.search(r'gpa: (0x[0-9a-f]+)', text)
            if m:
                pages[page] = int(m.group(1), 16)
                vm.hmp(f'pmemsave {pages[page]:#x} 0x1000 "{a.output / f"page-{page:08x}.bin"}"')
        (a.output / 'pages.json').write_text(json.dumps({f'{k:#x}': v for k, v in pages.items()}) + '\n')
        (a.output / 'ring0.json').write_text(json.dumps(ring0) + '\n')
        (a.output / 'v86.json').write_text(json.dumps(v86_raw) + '\n')
    finally:
        vm.close()
        disk.unlink(missing_ok=True)
    result = counts.most_common()
    (a.output / 'profile.json').write_text(json.dumps(result, indent=1) + '\n')
    for name, n in result[:45]:
        print(f'{n:5d} {name}')


if __name__ == '__main__':
    main()
