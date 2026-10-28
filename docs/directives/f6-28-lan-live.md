# f6-28: the f6-27 live LAN cases as runner multi-guest cases

- **Written:** 2026-10-28 20:18 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: branch `wt/integ8`. It contains:
  - f6-25, f6-26 and f6-27;
  - the fx-games20 pair runner `scripts/test/lan_runner.py`;
  - the authored LAN PEs `tests/win32/lan/`.
- **Inputs:**
  - `docs/validation/f6-27-lan-round2.md`, sections "Lead QEMU plan" and "Required live cases";
  - `docs/design/test-architecture.md`;
  - the owner limit: at most three QEMU guests at once, LAN tests only, each capped at `MemoryMax=1500M` and `MemorySwapMax=0`.

## Scope

1. **Make the authored fixtures reach the guests at run time.** `lan2-ipx.exe` and `lan2-directplay.exe` are built by `tests/win32/lan/build.py`. The canonical image must not change per test (test-architecture.md: no build variants, no whole-image copies).
   - Use the runner's existing run-time mechanisms: a per-run data disk or the overlay's test payload area, as other acceptance cases deliver their fixtures. Record the PE hashes in the result.
2. **Add a runner suite `lan-t23`** (or cases in `acc-t23` marked as LAN pairs; pick one and justify it) with:
   - Win32 AF_IPX host/join and swapped;
   - DirectPlay TCP/IP host/join;
   - DirectPlay IPX host/join;
   - one three-guest DirectPlay TCP/IP case over the multicast socket backend;
   - a framing case: A on Auto, B forced to 802.3 raw.

   Each case:
   - uses the profile NIC with distinct MACs and the socket netdev of the plan;
   - configures static IPv4 on the isolated segment through the normal Control Panel/settings path or a declared first-boot setting, never by editing the image;
   - waits for the PASS markers the fixtures print;
   - requires zero exit on every guest.
3. **Resources.**
   - Every guest runs in its own capped scope, holds a QEMU slot (`scripts/test/resources.py` slot locks) and has distinct serial and QMP paths.
   - Never more than three guests.
   - Passed overlays are deleted and no QEMU process is left.
4. **Host tests.** Cover case validation, the fixture delivery, netdev arguments, the slot limit and result aggregation with the fake QEMU of `tests/host/fake_qemu.py`.

## Acceptance

- The new host tests pass.
- `tests/host/test_acceptance.py`, `tests/host/test_runner*.py`, `tests/host/test_normal_boot.py` and `tests/host/test_dos_games20.py` pass.
- `bash scripts/test/acceptance.sh --list` (or the new suite's list) shows the cases.
- The lead runs the live cases on QEMU.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-f6-28` (branch `wt/f6-28`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/f6-28-report.md`. Leave the changes uncommitted.
