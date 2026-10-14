# Transient desktop UI (f2-64)

The Ciuki Blue theme, existing caption painter and approved Ciuki portrait
remain the visual source. Tango supplies the four message classes. The
toolkit and shell share measured UTF-8 wrapping and message body geometry;
buttons retain the toolkit's keyboard navigation and mnemonic behavior.
Warning uses the existing error sound because f4-12 has no warning event.

Research checked on 2026-10-14, before implementation:

- [Microsoft tooltip guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-tooltips-and-infotips)
  distinguishes hover descriptions/status from interactive notifications.
  Keep tips passive, delayed (600 ms), and anchored to their actual task or
  tray target. A changed target restarts the delay. Show complete task titles,
  volume state, network link/address, removable drive labels and clock date.
- [Microsoft balloon guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-balloons)
  describes contextual pointers and keeping a popup fully on screen.
  Attention notifications use a pointer toward the tray; ordinary updates use
  the same panel without a pointer. Neither takes focus. A click opens the
  associated panel or the full error message. Escape dismisses the notice.
- [Microsoft standard icons](https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-std-icons)
  distinguishes information, warning and error by context. CiukiOS also keeps
  its requested question class. All icons come from the existing Tango store;
  Tango 0.8.90 has no `dialog-question`, so the existing `dialog:question`
  mapping uses its `help-browser` question symbol.

Repository checks: `dialog.c` already owns application modality, focus and
class sounds; `shell.c` owns tray damage and hover timing. `/dev/net` provides
link and IPv4; `/dev/media` provides generations, letters and volume labels;
the mixer reports EC changes. There is no battery status ABI, so no battery
polling or invented battery values are added. These sources feed one bounded
notification slot: new events replace older ones, active attention messages
take priority over passive updates, volume repeats coalesce,
panels slide for 160 ms and expire after 8 seconds. Worker errors retain the
numeric errno alongside `strerror`, including `Read-only file system (30)`.

Host review material and checks are described in
`build/codex/f2-64-report.md`; PNGs are host renders, not guest evidence.
