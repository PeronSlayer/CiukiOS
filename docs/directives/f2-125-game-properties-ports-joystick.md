# f2-125 — Game properties: joystick, COM port and parallel port per game

- **Phase/model:** F2/F3, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f2-125`, holding f1-113 and f1-113b (joystick) with f1-112 (COM) merged in. It has an **in-progress merge of `wt/f1-114`** (parallel port) with conflicts.
- **Sources:** the lead decisions on f1-112, f1-113 and f1-114: one per-game panel for the physical COM port binding, the parallel port mode (INT 17h printer or raw lease) and the joystick choice.

## Required

1. **Resolve the merge**, editing files only; the lead commits. Conflicts are in `src/kernel/core/devices.c`, `src/kernel/vm/dosvm_bios.c`, `tests/host/devices_test.c`, `tests/host/abi_layout_test.c` and `docs/developer-api.md` (regenerate it).
   - The INT 11h equipment word must report both the virtual COM count of f1-112 and the LPT count of f1-114, from the VM's BIOS data area kept consistent by both. Prove it with a test.
   - Keep the device records of the game port, COM and LPT.
2. **Game properties panel.** In the desktop's existing per-game properties, without bringing back the removed DOS Box settings UI:
   - **Joystick:** the choice from f1-113b's persisted bindings;
   - **COM1–COM4:** bind to a physical port, a null device or none, with lease conflicts shown;
   - **LPT1:** INT 17h printer output to a file or printer device, raw lease, or none.

   Selections persist per game. A missing device falls back with a visible note, and changes apply at the next start of the game.
3. **Tests:** panel golden frames, persistence round trip, conflict display, the INT 11h word, and the existing f1-112/f1-113/f1-114 tests.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/f2-125-report.md`.
