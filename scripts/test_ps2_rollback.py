#!/usr/bin/env python3
"""Focused execution of production PS/2 rollback; modeled 8042, not hardware.

Uses the existing instruction-level controller fixture, adding failure-state
assertions absent from its older boot checks. The optional old kernel must
demonstrate the original AUX/IRQ rollback defect.
"""
import argparse
import json
from pathlib import Path
from test_ps2_boot import Controller, execute, symbol
from unicorn.x86_const import UC_X86_REG_EFLAGS


class FaultController(Controller):
    def __init__(self, fault, translation, keyboard_irq):
        super().__init__(translation=translation)
        self.config = (self.config & ~1) | keyboard_irq
        self.fault = fault
        self.busy = 0
        self.fired = False
        self.command_reads = 0
        if fault == 'initial-busy':
            self.busy = 65535
        elif fault == 'defaults-rejected':
            self.reject = 0xF6
        elif fault == 'enable-rejected':
            self.reject = 0xF4

    def read(self, uc, port, size, data):
        if port == 0x64 and self.busy:
            self.busy -= 1
            return 2
        return super().read(uc, port, size, data)

    def write(self, uc, port, size, value, data):
        pending = self.pending
        if port == 0x64 and value == 0x20:
            self.command_reads += 1
            self.no_reply = self.fault == 'first-read-timeout' and self.command_reads == 1
        super().write(uc, port, size, value, data)
        if port == 0x64 and value == 0x60 and self.fault == 'config-write-timeout' and not self.fired:
            self.fired = True
            self.busy = 65535
        if port == 0x60 and pending == 0xD4 and self.fault == 'bad-ack-source':
            self.queue.clear()
            self.queue.append((False, 0xFA))


def check(kernel, listing, fault, translation, keyboard_irq, flags):
    controller = FaultController(fault, translation, keyboard_irq)
    cpu, base = execute(kernel, listing, controller, 'ps2_mouse_init', flags=flags)
    ready = cpu.mem_read(base + symbol(listing, 'mouse_hw_ready'), 1)[0]
    assert ready == 0, 'failed mouse exposed as ready'
    assert cpu.reg_read(UC_X86_REG_EFLAGS) & 1, 'failure returned success'
    assert cpu.reg_read(UC_X86_REG_EFLAGS) & 0x200 == flags & 0x200, 'caller IF changed'
    assert controller.config & 0x40 == translation, 'BIOS translation changed'
    assert controller.config & 1, 'keyboard IRQ disabled'
    assert not controller.config & 0x10, 'keyboard clock disabled'
    assert controller.config & 0x20, 'failed AUX clock left enabled'
    assert not controller.config & 2, 'failed AUX IRQ left enabled'
    assert not controller.pic[0x21] & 2, 'PIC keyboard IRQ masked'
    assert controller.pic[0xA1] & 0x10, 'PIC failed AUX IRQ unmasked'
    assert not controller.queue, 'stale byte blocks shared output buffer'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--listing', type=Path, required=True)
    parser.add_argument('--old-kernel', type=Path)
    parser.add_argument('--old-listing', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    kernel, listing = args.kernel.read_bytes(), args.listing.read_text()
    cases = []
    for fault in ('initial-busy', 'first-read-timeout', 'config-write-timeout',
                  'defaults-rejected', 'enable-rejected', 'bad-ack-source'):
        for translation in (0, 0x40):
            for keyboard_irq in (0, 1):
                for flags in (2, 0x202):
                    check(kernel, listing, fault, translation, keyboard_irq, flags)
                    cases.append([fault, translation, keyboard_irq, flags])
    old_error = None
    if args.old_kernel:
        try:
            check(args.old_kernel.read_bytes(), args.old_listing.read_text(),
                  'defaults-rejected', 0x40, 1, 0x202)
        except AssertionError as exc:
            old_error = str(exc)
        assert old_error, 'old-kernel negative control unexpectedly passed'
    args.report.write_text(json.dumps({'passed': len(cases), 'cases': cases,
                                      'old_failure': old_error}, indent=2)+'\n')
    print(f'PASS {len(cases)} production rollback cases; old failure: {old_error}')


if __name__ == '__main__':
    main()
