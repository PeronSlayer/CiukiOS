# f6-28 authored LAN runner

## Research and implementation decision

Primary sources checked before implementation:

- [QEMU network options](https://www.qemu.org/docs/master/system/invocation.html#network-options):
  socket listen/connect connects two Ethernet backends; multicast shares an
  Ethernet bus among several guests. Use loopback listen/connect for pairs and
  loopback multicast for the three-guest case. Do not use user-mode IPv4 NAT.
- [QEMU disk images](https://www.qemu.org/docs/master/system/images.html):
  qcow2 backing files preserve the canonical image; snapshot mode can put
  temporary writes in RAM-backed `/tmp`. Reuse the runner's on-disk overlays
  and offline writable partition export, never snapshot or whole-image copies.
- [QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html): user items
  use `opt/` names. These cases need the ordinary desktop, so they use the
  acceptance input driver without a test-request fw_cfg item.

Repository comparison: `dos_games.stage` already delivers fixtures through a
FUSE export of the private overlay. `net/settings.c` loads `NETWORK.INI` using
the bounded, identity-bound codec in `net/config.c`; `IPX.CFG` uses the normal
IPX setting parser. The declared first-boot settings use those existing loaders
with static 192.0.2.1--3/24, no gateway or DNS, and a unique PCI/MAC identity.
Only the private overlay receives these initial settings and the authored PEs;
the canonical image is never edited. This does not qualify runtime Settings
authorization or the Control Panel persistence/reboot case.

`lan-t23` is separate from `acc-t23`: it explicitly requires an isolated segment
and concurrent peers, whereas the hardware trial list remains independently
selectable. Every guest has one slot, one 1500M/0 scope, one overlay and its own
serial/QMP directory. The parent reserves all four possible slot locks before launching
any guest so another runner cannot exceed the three-guest LAN allowance.
Three-guest cases require `CIUKI_RUNNER_SLOTS=3` (or 4); fewer slots refuse.

Fixtures are built explicitly with `tests/win32/lan/build.py`, never by a test.
The runner checks the build manifest against the current fixture/build source
hashes (stale builds are `not_run`) and records both PE hashes, bytes delivered,
first-boot settings, role, backend, MAC, image hash and each child result.
All desktops must reach the launch barrier before the host starts. Joiners
start only after the host fixture's ready marker. PASS plus a matching Win32
process exit code of zero and clean QEMU teardown are mandatory on every guest.
The three-guest fixture requires chat from both other player IDs.

`kernel32!AllocConsole` writes to a private temporary file, rather than the
serial port. The authored fixtures therefore accept explicit `--ciuki-serial`
to mirror their bounded lines through existing diagnostic syscall 2, whose
kernel-added `[user <task>]` prefix is required by the recipes. The Windows
reference run omits this option and retains ordinary `WriteFile` output.
No kernel diagnostic API or public header changes are needed.

## Lead live qualification

After host review and the canonical build, build the authored PEs explicitly,
then list and run the suite:

```sh
nice -n 19 python3 tests/win32/lan/build.py
bash scripts/test/acceptance.sh --suite lan-t23 --list
CIUKI_RUNNER_SLOTS=3 bash scripts/test/acceptance.sh --suite lan-t23
```

The implementer's host tests use `tests/host/fake_qemu.py`; they establish no
live protocol, image, hardware or Windows interoperability result. The other
f6-27 cases (DOS games, runtime framing persistence/permissions, loss injection,
router prevalence and licensed Windows reference) remain separate lead work.
