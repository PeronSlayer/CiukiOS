#!/usr/bin/env python3
"""Compile and link video + peripheral cores together; no guest-I/O claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include "vga_presenter.h"
#include "guest_peripherals.h"
#include "guest_peripheral_scheduler.h"

static cvga_state video;
static cvp_state presenter;
static cvgp_state devices;

int vm_devices_link_probe(void)
{
    uint32_t scan = 0;
    cvga_init(&video);
    cvp_init(&presenter);
    cvgp_init(&devices);
    if (cvgp_begin(&devices, 1, CVGP_CAP_KEYBOARD,
                  CVGP_CAP_KEYBOARD, 0) != CVGP_OK) return 1;
    if (cvgp_set_focus(&devices, 1, 1) != CVGP_OK) return 2;
    if (cvgp_key_event(&devices, 1, 0x39, 0, 1) != CVGP_OK) return 3;
    if (cvgp_io_read(&devices, 1, 0x60, 1, 0, &scan) != CVGP_IO_OK)
        return 4;
    if (scan != 0x39) return 5;
    if (cvgp_end(&devices, 1) != CVGP_OK) return 6;
    return 0;
}
#ifndef VM_TARGET
int main(void) { return vm_devices_link_probe(); }
#endif
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/tests/vm-device-link')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report_path = out / 'report.json'
    if report_path.exists():
        parser.error('Choose a fresh output directory to retain prior evidence')
    probe = out / 'combined.c'
    probe.write_text(PROBE)
    vm = ROOT / 'src/vm'
    sources = [vm / name for name in
               ('virtual_vga.c', 'vga_presenter.c', 'guest_peripherals.c')]
    headers = [vm / name for name in
               ('virtual_vga.h', 'vga_presenter.h', 'guest_peripherals.h',
                'guest_peripheral_scheduler.h')]
    host = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
            '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-I' + str(vm), str(probe),
            *map(str, sources), '-o', str(out / 'combined')]
    subprocess.run(host, check=True)
    subprocess.run([str(out / 'combined')], check=True)
    watcom = Path(os.environ.get('WATCOM', '/opt/watcom'))
    compiler = watcom / 'binl64/wcc386'
    if not compiler.exists():
        compiler = watcom / 'binl/wcc386'
    objects = []
    for source in [probe, *sources]:
        obj = out / (source.stem + '.obj')
        subprocess.run([str(compiler), '-zq', '-bt=dos', '-mf', '-3r',
                        '-ecc', '-zl', '-s', '-w4', '-we', '-dVM_TARGET',
                        '-i=' + str(watcom / 'h'), '-i=' + str(vm),
                        '-fo=' + str(obj), str(source)], check=True)
        objects.extend(['file', str(obj)])
    target = [str(compiler.with_name('wlink')), 'option',
              'quiet,nodefaultlibs,start=_vm_devices_link_probe',
              'format', 'raw', 'bin', 'disable', '1014',
              'name', str(out / 'combined.bin'), *objects]
    subprocess.run(target, check=True)
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()
    report = dict(result='PASS',
                  scope='Actual combined headers/objects, sanitizer host call, freestanding target link',
                  guest_execution=False, hardware_audio=False,
                  host_command=host, target_link_command=target,
                  sources={str(p.relative_to(ROOT)): sha(p)
                           for p in [*sources, *headers, Path(__file__).resolve()]},
                  probe_sha256=sha(probe), target_sha256=sha(out / 'combined.bin'))
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: video and peripheral headers, host execution and freestanding combined link')


if __name__ == '__main__':
    main()
