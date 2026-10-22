# f1-106b — EHCI transaction translators (USB 2.0 hubs with keyboards and mice) and interrupt-driven idle HID

- **Phase/model:** F1, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f1-106b` from `wt/f1-106`.
- **Sources:**
  - **f1-106 report:** full- and low-speed devices behind a high-speed hub are refused, because there is no transaction translator support. Retro PCs of 2002–2004 commonly have USB 2.0 hubs with a keyboard or mouse behind them.
  - **Lead measurement (2026-10-23 00:20, `tests/host/usb/run.sh`):** an idle USB host performs about 1004 worker wakes per second (4 scans and 1000 HID turns). That is a 1 ms HID fallback poll, against the Windows 98 model in which an idle system is not scheduled (`win98-class-resource-management.md`).

## Required

1. **Split transactions** (EHCI 1.0 §4.12, USB 2.0 §11.14–11.21): start-split and complete-split scheduling for control, bulk and interrupt endpoints behind a TT, with periodic budget accounting per microframe and the hub's TT think time. Errors and timeouts recover through the hub's Clear_TT_Buffer and Reset_TT. Low- and full-speed HID and mass storage work behind a high-speed hub.
2. **Interrupt-driven HID** on UHCI, OHCI and EHCI: interrupt transfers complete through the controller's IOC/completion interrupt and wake the worker only on a completion or a deadline. No 1 ms fallback poll while the controller interrupts are qualified; keep a bounded, recorded fallback only for a controller whose interrupt is proved broken. Input latency stays within the existing input budget.
3. **Tests:**
   - EHCI model: TT split scheduling, budgets, errors, Clear_TT_Buffer, a mixed tree with a high-speed hub carrying a low-speed keyboard, a full-speed mouse and a full-speed stick;
   - an idle-wake test: no periodic wakes on an idle host with HID attached, and a key press delivered within one completion;
   - the existing USB suites stay green.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (EHCI 1.0, USB 2.0, UHCI 1.1, OHCI 1.0a). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-106b-report.md`.
