# f6-22: e100 receive status, network state records, complete NetSurf lines

Status (2026-10-22): host fixtures pass; **T23 confirmation pending** (no
image, QEMU or hardware run in this delivery).

## Evidence (lead, directive `docs/directives/f6-22-*.md`)

T23 trial of image 5a10eef4 (serial copy `build/codex/t23-5a10eef4-serial.txt`):

- `[net] driver=e100 mac=00:d0:59:b6:43:8f link=up irq=11 mode=irq
  state=running reason=admission`, then nothing from the network stack.
  `reason=admission` is the driver's initial reason string, kept after a
  successful attach; it is not a failure.
- `[console] [netsurf] layout slow elapsed_us=` and `[console] [netsurf]
  exit reason=` with the values on the following `[console]` records:
  `3300000`, ` class=`, `local`, ` accepted=`, `1`, and for the exit line
  `desktop-close-request`, ` detail=`, `0`, ..., ` keepalive_pumps=`, `203`.
  Reassembled: `[netsurf] layout slow elapsed_us=3300000 class=local
  accepted=1` and `[netsurf] exit reason=desktop-close-request detail=0
  layout_rejections=0 layout_max_us=3300000 local_max_us=3300000
  remote_max_us=0 input_overflows=0 keepalive_pumps=203`. The values were
  split, not lost: the owner closed the window, and the welcome page took
  3.3 s to lay out on the T23 (local content, accepted).

## Diagnosis 1: the e100 dropped every received frame on physical hardware

The stack was started (no `[net] disabled` record; `net_stack_init` runs
before `net_stack_driver_start` attaches the running adapter) and DHCP was
started on link up. The receive path refused the replies:
`e100_poll` accepted an RFD only when `status & 0x2000` (OK) and
`!(status & 0x0fff)`, treating all of status bits 12:0 as errors.

- [Intel 8255x Open Source Software Developer Manual](https://media.digikey.com/pdf/Data%20Sheets/Intel%20PDFs/8255x%2C82562%20Ethernet%20Controller%20Developer%20Manual.pdf),
  section 6.4.3.1.2, table 49 "RFD Status Bit Descriptions": bit 11 CRC,
  bit 10 alignment, bit 9 no resources, bit 8 DMA overrun, bit 7 too short
  and bit 4 RX_ER describe damaged frames (most only in save-bad-frames
  mode); **bit 5 (Type/Length) is set for every type frame** (type field 0
  or > 1500: every Ethernet II frame, so IPv4 and ARP), **bit 1 (IA match)
  is 1 for every frame not addressed to the individual address** (broadcast,
  multicast), bit 2 reports no filter match (promiscuous), bit 0 is receive
  collision (82557/8) or TCO indication (82559). Bits 12, 6 and 3 are
  reserved.
- [Linux e100.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/intel/e100.c),
  `e100_rx_indicate`: a completed RFD is dropped only when `cb_ok` (0x2000)
  is clear; no other status bit is tested.
- [QEMU eepro100.c](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/eepro100.c),
  `nic_receive`: status `0xa000`, `|0x0002` for broadcast/multicast,
  `|0x0004` for promiscuous unicast, no Type/Length bit.

So on the T23 every frame (DHCP OFFER/ACK, ARP) was counted in `rx_errors`
and never reached lwIP; on QEMU's i82559er every broadcast frame (slirp's
DHCP replies) was dropped too. The existing QEMU ARP case passed because
slirp's ARP reply is unicast; the DHCP/browser cases use e1000. The host
register model wrote `0xa000` for every frame, which hid the defect.

Decision: accept C+OK frames unless one of the table-49 damage bits
(`0x0f90`: 11, 10, 9, 8, 7, 4) is set; bits 5, 2, 1 and 0 are information.
The host model now writes the status each device writes (table 49 on the
physical profile, `nic_receive` on the QEMU profile);
`test_e100_rfd_status` delivers a broadcast DHCP OFFER, a unicast ARP reply,
an IEEE 802.3 length frame and the informational bits to the ingress
callback, and counts every damage bit as an error without a fault. With the
old mask the existing `test_e100_rfd_flags` physical case already fails
under the corrected model (checked).

## Diagnosis 2: no record of the stack's state

`stack.c` logged nothing after the driver record. Added, all `[net]`
protected records (`klog`; the stack line via `klog_periodic`, still a
protected class because `[net]` is not a budgeted statistic prefix):

- `[net] dhcp state=start|start_failed iface=en0 mac=… error=N`;
- `[net] dhcp state=<discover|offer|checking|bound|renewing|rebinding|
  rebooting|backing_off|off|init> iface=en0 ip=… gw=… dns=…[,…] …
  tries=N suppressed=N` on each lwIP DHCP state change (`offer` and
  `checking` show the offered address and the server; `bound` adds mask and
  lease). Changes are observed after each input frame, in the netif status
  callback and in a watch pass every 250 ms;
- `[net] dhcp state=timeout phase=… elapsed_ms=… rx=… rx_errors=… tx=…
  in=… out=…` when an acquisition has not completed after 30 s, at most once
  a minute: driver and stack frame counters side by side show a receive
  path that drops frames (the T23 case would read `rx=0 rx_errors=N`);
- `[net] resolver first_answer|first_failure ip=… servers=…,…
  elapsed_ms=… generation=N`, once each per DNS-server generation; the
  looked-up name is not recorded;
- `[net] stack ifaces=N enX link=… ip=… gw=… dns=… dhcp=… rx=… tx=…
  errors=… dropped=… in=… out=… dns_ok=N dns_fail=N`, every 10 s for the
  first two minutes, then every 60 s, skipped while nothing changed (at
  most every 5 min then).

lwIP restarts DISCOVER at once after a NAK
([pinned dhcp.c](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/core/ipv4/dhcp.c),
`dhcp_handle_nak`), so a NAKing server drives offer/discover in a tight
loop: the host harness produced 2,136 records before the limit. State
records other than `bound` spend a token (burst 16, one per second);
suppressed transitions are counted in the next record. The watch pass is a
preallocated tcpip callback message posted by the NIC worker
(`net_stack_watch`), not an lwIP timeout: `sys_timeout` asserts when the
timeout pool is empty, which would stop the core.

## Diagnosis 3: split stderr lines

Ciuki's newlib (4.5.0.20241231, pinned SDK) opens stderr unbuffered and
read/write: `stderr_init` uses `__SRW | __SNBF` ("POSIX requires stderr to
be opened for reading and writing", `libc/stdio/findfp.c`). `vfprintf`'s
`_UNBUF_STREAM_OPT` shortcut (`__sbprintf`, one buffered write per call)
applies only to `(__SNBF|__SWR)` streams without `__SRW`
(`libc/stdio/vfprintf.c`), so stderr takes `__sfvwrite_r`, which writes
each formatted piece separately (`libc/stdio/fvwrite.c`, unbuffered
branch). The kernel console sink (`src/kernel/proc/devnodes.c`,
`device_write`) frames each `write(2)` as one `[console]` record and ends a
record without a newline, which is what the serial shows. No byte was
mangled or lost.

Decision (port scope): NetSurf makes stderr line-buffered at start (C17
7.21.3p7 and [POSIX stdin/stdout/stderr](https://pubs.opengroup.org/onlinepubs/9799919799/functions/stdin.html)
require only that stderr is not fully buffered), so every complete line,
NetSurf's own NSLOG output included, is one write; the record lines
(`layout slow`, `exit reason`) are formatted into one buffer and written
with one `write(2)` regardless of stdio state (`sdk/netsurf/port/logline.h`).
Other native programs keep the split (`desktop: present failed (errno=`
in the same log); an SDK-wide fix (newlib or crt) is a lead decision.

## NetSurf network state and NETSURF.LOG

- Failure pages (offline, DNS failure, refused/timeout/connection errors)
  and the HTTPS readiness page append the network state from `/dev/net` and
  the browser's last lookup: `Network: no address yet (e100, link up);
  waiting for a DHCP server.`, `Network: address A.B.C.D (e100); DNS ok.`,
  `… DNS lookups failing.`, `… DNS not tried yet.`, or no adapter /
  stopped / no link.
- The exit line gains `network=<unknown|no-adapter|stopped|no-link|
  no-address|address|dns-ok|dns-failed>` and is appended, with a UTC stamp,
  to `C:\SYSTEM\LOGS\NETSURF.LOG` (`/system/logs/NETSURF.LOG`; restarted
  past 64 KiB). An early exit (`die()`, bad arguments) still logs a line
  from an `atexit` handler.

## Host verification (this worktree, 2026-10-22)

See `build/codex/f6-22-report.md` for commands and results.
