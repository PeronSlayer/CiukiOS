# Runner USB input configuration

The runner accepts `usb` and `i8042` on profiles and cases. Case values replace
profile values; an empty USB list removes the profile's USB controllers. Defaults
remain an enabled i8042 and no explicitly added USB controllers. USB entries
declare only `controller` (`uhci` or `ohci`) and `devices` (`kbd`, `mouse`, or
`tablet`). Controllers receive stable list-order IDs `usb0`, `usb1`, etc., and
devices explicitly use that controller's `.0` bus. Evidence records the effective
configuration, including empty USB lists and the i8042 boolean.

Research before implementation (2026-10-13):

- [QEMU USB documentation](https://www.qemu.org/docs/master/system/devices/usb.html)
  describes the HID devices and explicit controller/bus attachment. Use
  `piix3-usb-uhci` and `pci-ohci`, with `usb-kbd`, `usb-mouse`, or `usb-tablet`.
- [QEMU PC machine source](https://github.com/qemu/qemu/blob/master/hw/i386/pc.c)
  exposes the boolean i8042 machine property and disables automatic vmport when
  i8042 is disabled. Before building a command with `i8042=off`, query the actual
  executable with `-machine <machine>,help`; refuse if the boolean property is
  absent or the query fails. This help query exits without booting a guest.
  The installed package is `qemu-system-x86 11.1.1-4` (package-manager query),
  and its binary contains the i8042 property's upstream description (static
  `strings` inspection).
  This worktree runs no QEMU executable, including help queries; capability
  execution is covered with mocks and the scripted host fixture here.
- [QMP input-send-event](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
  and [input routing source](https://github.com/qemu/qemu/blob/master/ui/input.c)
  select the first active eligible handler for each event kind when no routing
  is specified. `device` names a display for routing, not a USB controller.
- [QEMU HID source](https://github.com/qemu/qemu/blob/master/hw/input/hid.c)
  activates a USB keyboard at initialization; a USB pointer activates when the
  guest polls it. Thus with USB and PS/2 present, USB keyboard initialization
  takes keyboard precedence, and the USB mouse takes relative/button precedence
  after polling, subject to later handler activation. Events are not broadcast
  to both. A tablet accepts absolute motion, whereas the existing runner sends
  relative motion; tablet configuration alone does not establish mouse input
  acceptance. The USB acceptance cases use a keyboard and relative mouse with
  i8042 disabled to remove ambiguity.

The repository's `Actions.step` calls `QMP.input_events` with the existing qcode,
relative-motion and button unions, without a display-routing parameter. Preserve
this path and its READY/ARM pacing; USB host controllers require no different
QMP request. This establishes the host request contract only. Firmware/native
input delivery must still be demonstrated on the current image by the lead.

`f1-usb` requires `f0-smoke`, then runs the existing desktop-shell contract on
`qemu-desktop-2002` with UHCI and OHCI respectively. Although named for F1 USB
drivers, these cases select `f2:crash-isolation ... server=desktop`, retaining
the shell's keys, clicks, fault isolation, live screenshot and guest predicates.
They additionally require line-anchored `expected.serial_patterns` in the raw,
bounded serial transcript: the selected controller, two device enumeration lines
with a class field, and keyboard/mouse attachment. These diagnostic records are
supplementary evidence and never replace CIUKI_TEST framing or terminal checks.
Missing serial patterns fail the run; matched lines are retained in result.json.
Image/firmware hashes, overlays, T1 checks and test selectors are unchanged.

Host validation and integration status are recorded in
`build/codex/runner-02-report.md`. QEMU acceptance remains for the lead.
