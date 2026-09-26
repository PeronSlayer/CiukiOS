#!/usr/bin/env python3
"""Execute the assembled kernel's PS/2 routines against an interleaved 8042.

Run after build_full.sh, or point --kernel/--listing at a saved older build:
  uv run --with unicorn==2.1.4 python scripts/test_ps2_boot.py

This exercises real machine code, including IRQ12 and error exits. QEMU's
ordinary idle-at-boot devices do not expose these controller ordering races.
"""

import argparse
from collections import deque
from pathlib import Path
import re
import struct

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN
from unicorn.x86_const import (
    UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_AX, UC_X86_REG_CS,
    UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_SP,
    UC_X86_REG_IP, UC_X86_REG_EFLAGS,
)


def symbol(listing, name):
    lines = listing.splitlines()
    for i, line in enumerate(lines):
        if re.search(r"\b" + re.escape(name) + r"(?=:|\s+d[bwd]\b)", line):
            for instruction in lines[i:]:
                match = re.match(r"\s*\d+\s+([0-9A-F]{8})\s", instruction)
                if match:
                    return int(match[1], 16)
    raise AssertionError(f"missing assembled symbol: {name}")


class Controller:
    def __init__(self, *, translation=0x40, race=None, reject=None,
                 no_reply=False):
        self.config = translation | 0x05  # BIOS system flag and keyboard IRQ
        self.translation = translation
        self.race = race
        self.reject = reject
        self.no_reply = no_reply
        self.queue = deque()
        self.pending = None
        self.pic = {0x21: 0xF8, 0xA1: 0xFF}
        self.reads = []
        self.mouse_reporting = False

    def inject_race(self):
        # A device byte arriving after the initial flush, before command 20h.
        if self.race == "keyboard" and not self.config & 0x10:
            self.queue.append((False, 0x1E))
        elif self.race == "mouse" and not self.config & 0x20:
            self.queue.extend((True, b) for b in (0x38, 0xFF, 0xFF))
        self.race = None

    def read(self, _uc, port, size, _data):
        assert size == 1
        if port == 0x64:
            return (1 | (0x20 if self.queue[0][0] else 0)) if self.queue else 0
        if port == 0x60:
            assert self.queue, "read of an empty controller buffer"
            source, value = self.queue.popleft()
            self.reads.append((source, value))
            return value
        return self.pic.get(port, 0)

    def write(self, _uc, port, size, value, _data):
        assert size == 1
        if port == 0x64:
            self.pending = None
            if value == 0xAD:
                self.config |= 0x10
            elif value == 0xAE:
                self.config &= ~0x10
            elif value == 0xA7:
                self.config |= 0x20
            elif value == 0xA8:
                self.config &= ~0x20
                self.inject_race()
            elif value == 0x20:
                if not self.no_reply:
                    self.queue.append((False, self.config))
            elif value in (0x60, 0xD4):
                self.pending = value
            else:
                raise AssertionError(f"unexpected controller command {value:02x}")
        elif port == 0x60:
            if self.pending == 0x60:
                self.config = value
            elif self.pending == 0xD4:
                if self.reject == "absent":
                    pass
                elif value == self.reject:
                    self.queue.append((True, 0xFE))
                else:
                    self.queue.append((True, 0xFA))
                    if value == 0xF6:
                        self.mouse_reporting = False
                    elif value == 0xF4:
                        self.mouse_reporting = True
            else:
                raise AssertionError("mouse init wrote an unsolicited keyboard command")
            self.pending = None
        elif port in self.pic:
            self.pic[port] = value
        else:
            assert port in (0x20, 0xA0) and value == 0x20, "unexpected I/O"


def execute(kernel, listing, controller, routine, *, irq=False, flags=0x202):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    segment, base, stop = 0x900, 0x9000, 0xFF00
    uc.mem_write(base, kernel)
    for reg in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES):
        uc.reg_write(reg, segment)
    uc.reg_write(UC_X86_REG_SS, 0x7000)
    uc.reg_write(UC_X86_REG_SP, 0xFFF0)
    frame = struct.pack("<HHH", stop, segment, flags) if irq else struct.pack("<H", stop)
    uc.mem_write(0x7FFF0, frame)
    uc.reg_write(UC_X86_REG_EFLAGS, flags)
    uc.reg_write(UC_X86_REG_AX, 0xF4)
    uc.hook_add(UC_HOOK_INSN, controller.read, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, controller.write, None, 1, 0, UC_X86_INS_OUT)
    uc.emu_start(base + symbol(listing, routine), base + stop, count=2000000)
    assert uc.reg_read(UC_X86_REG_IP) == stop, "routine did not terminate within its budget"
    return uc, base


def check_init(kernel, listing, **options):
    flags = options.pop("flags", 0x202)
    controller = Controller(**options)
    uc, base = execute(kernel, listing, controller, "ps2_mouse_init", flags=flags)
    assert controller.config & 1, "mouse initialization disabled keyboard IRQ1"
    assert not controller.config & 0x10, "mouse initialization left keyboard clock disabled"
    assert controller.config & 0x40 == controller.translation, "BIOS keyboard translation corrupted"
    assert not controller.pic[0x21] & 2, "keyboard IRQ1 remains masked"
    assert uc.reg_read(UC_X86_REG_EFLAGS) & 0x200 == flags & 0x200, "caller's IF changed"
    expected_ready = not options.get("reject") and not options.get("no_reply")
    ready = uc.mem_read(base + symbol(listing, "mouse_hw_ready"), 1)[0]
    assert ready == int(expected_ready), f"unexpected mouse readiness: {ready}"
    assert bool(uc.reg_read(UC_X86_REG_EFLAGS) & 1) != expected_ready, "wrong error status"
    if expected_ready:
        assert controller.mouse_reporting, "mouse stream was not enabled"
        assert not controller.pic[0xA1] & 0x10, "mouse IRQ12 remains masked"


def check_irq_keyboard(kernel, listing):
    controller = Controller()
    controller.queue.append((False, 0x1E))
    execute(kernel, listing, controller, "irq12_mouse_handler", irq=True)
    assert list(controller.queue) == [(False, 0x1E)], "IRQ12 consumed the keyboard's scan code"


def check_irq_mouse(kernel, listing):
    controller = Controller()
    controller.queue.append((True, 0x08))
    uc, base = execute(kernel, listing, controller, "irq12_mouse_handler", irq=True)
    assert not controller.queue, "IRQ12 did not consume a real AUX byte"
    assert uc.mem_read(base + symbol(listing, "mouse_packet_index"), 1) == b"\1"


def check_ack_keyboard(kernel, listing):
    controller = Controller()
    controller.queue.append((False, 0x1E))
    uc, _ = execute(kernel, listing, controller, "ps2_mouse_write")
    assert list(controller.queue)[:1] == [(False, 0x1E)], "ACK polling consumed a keyboard byte"
    assert uc.reg_read(UC_X86_REG_EFLAGS) & 1, "pending keyboard byte was accepted as a mouse ACK"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=Path("build/full/obj/ciukidos.sys"))
    parser.add_argument("--listing", type=Path, default=Path("build/full/obj/ciukidos.lst"))
    args = parser.parse_args()
    kernel, listing = args.kernel.read_bytes(), args.listing.read_text()
    cases = [
        ("idle", lambda: check_init(kernel, listing)),
        ("keyboard arriving during init", lambda: check_init(kernel, listing, race="keyboard")),
        ("TrackPoint packet during init", lambda: check_init(kernel, listing, race="mouse")),
        ("BIOS without translation", lambda: check_init(kernel, listing, translation=0)),
        ("caller interrupts disabled", lambda: check_init(kernel, listing, flags=2)),
        ("mouse rejects defaults", lambda: check_init(kernel, listing, reject=0xF6)),
        ("mouse rejects enable", lambda: check_init(kernel, listing, reject=0xF4)),
        ("mouse absent", lambda: check_init(kernel, listing, reject="absent")),
        ("controller read times out", lambda: check_init(kernel, listing, no_reply=True)),
        ("spurious IRQ12 with keyboard byte", lambda: check_irq_keyboard(kernel, listing)),
        ("IRQ12 with mouse byte", lambda: check_irq_mouse(kernel, listing)),
        ("keyboard while polling mouse ACK", lambda: check_ack_keyboard(kernel, listing)),
    ]
    failures = 0
    for name, check in cases:
        try:
            check()
            print(f"[ps2-boot] PASS {name}")
        except AssertionError as exc:
            failures += 1
            print(f"[ps2-boot] FAIL {name}: {exc}")
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
