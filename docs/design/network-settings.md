# Network settings (f2-100)

The syscall-90 record separates saved intent from live IPv4 and lease state.
Only the supervisor, or the desktop holding both display and input grants,
can delegate network authority to the exact desktop and Control Panel paths.
Launching another program does not confer it. `/dev/net` stays read-only.
This is the f2-100 implementation proposal for lead review, replacing the
unimplemented F6-03 control/persistence sketches in network-contract.md.
It uses the existing exact-path supervisor capability pattern (also used by
program management) rather than adding a transferable authority descriptor.

Research: Windows 98 Resource Kit, chapter 15, local read-only
`build/rskt/wrkc15_3.htm` (Configuring IP Addresses, DNS and multihoming) and
`wrkc15_7.htm` (Resources and Configuring Advanced Properties for Network
Adapters). The panel separates adapter information, TCP/IP properties and
status; resource assignments are observations, and advanced choices depend
on the driver's capabilities. CiukiOS keeps one default route and one system
resolver. Saved secondary-interface DNS settings are retained; only the
selected primary supplies the global resolver.

Primary references:

- [RFC 2131](https://www.rfc-editor.org/rfc/rfc2131), sections 4.4.4–4.4.6:
  renew is distinct from release; released addresses must no longer be used.
- [lwIP DHCP](https://www.nongnu.org/lwip/2_1_x/group__dhcp4.html) and
  [DNS](https://www.nongnu.org/lwip/2_1_x/group__dns.html): mutate on the
  TCP/IP core; release/stop before static addressing, use the system resolver,
  expose three DNS slots by increasing its bounded compile-time capacity.
- [Intel 8255x manual](https://media.digikey.com/pdf/Data%20Sheets/Intel%20PDFs/8255x%2C82562%20Ethernet%20Controller%20Developer%20Manual.pdf),
  MDI and configuration byte 19: PHY BMCR selects automatic or forced media;
  the MAC follows the PHY duplex indication.
- [Intel 8254x manual](https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf),
  sections 8.6 and 13: forced PHY and MAC speed/duplex must agree. Report
  negotiated speed from STATUS; never infer a successful physical link from
  the configured selection. M88 forced 10/100 disables automatic crossover
  and latches the change with a bounded PHY software reset. Auto mode restores
  crossover and negotiation; reset completion also restores the 25 MHz
  transmit clock and CRS-on-transmit. These steps are cross-checked against Intel's
  [upstream e1000 implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000/e1000_hw.c)
  (`e1000_phy_force_speed_duplex`) and its
  [register definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/e1000/e1000_hw.h).
  STATUS.TBIMODE controls fiber capabilities; PCI device 100F is not treated
  as a fiber selector.

Persistence follows the bounded kernel settings-file pattern used by audio:
`C:\SYSTEM\NETWORK.INI`, validated as a complete snapshot before publication.
The kernel commits `NETWORK.NEW`, atomically replaces the canonical file through
the existing native rename path, then flushes the drive. Settings bind to the
PCI vendor/device and permanent MAC, not the enumeration index. The two named
sections are storage slots, not persistent interface IDs. Failed pre-publication
writes preserve the previous file. A final flush failure is reported as an
uncertain durable result, never as a successful save.
Missing configuration uses DHCP. Invalid, oversized or unreadable configuration
uses DHCP and records the fallback. SET reports storage errors to its caller;
no save failure is reported as success. Boot reads precede stack attachment and
the first DHCP packet. Interface disable is administrative: the stack goes down,
releases its lease, rejects transmit/ingress and retains saved intent; hardware
DMA ownership and quarantine rules remain with the NIC worker.
The NIC worker drops queued native traffic and revokes the DOS bridge carrier
when an interface is administratively disabled.

Syscall 90 uses B=operation, C=record pointer, D=400; other registers are zero.
The 400-byte `ciuki_net_control` contains version/size/index/count (16 bytes),
`ciuki_net_config` (232 bytes), name/driver/MAC (32 bytes), ten u32 adapter
fields (40 bytes), address/mask/gateway/three DNS/lease/remaining (32 bytes),
four u64 counters (32 bytes), generation and three zero reserved u32s (16 bytes).
Operations are GET=0, SET=1, RENEW=2, RELEASE=3. SET changes enabled/DHCP,
static IPv4, DNS, host/domain and media as one configuration; renew/release
are transient. Mutations require the generation returned by GET (EBUSY if
stale). The syscall copies the input and pins output before side effects.

NETWORK.INI is strict ASCII: `version=1`, then `[en0]` and `[en1]`, each with
enabled/dhcp/speed/duplex/address/mask/gateway/dns1/dns2/dns3/hostname/domain/
vendor/device/mac, then a decimal FNV-1a `checksum` over all preceding bytes.
Every line ends in LF; addresses are dotted decimal and MACs lowercase hex.
All fields are required exactly once, unknown fields and malformed identities
are refused; maximum file size is 2047 bytes. Leases and observations are not
saved. Disabled ports retain their configured intent. Masks must be contiguous,
static addresses usable unicast hosts, gateways distinct hosts on the same
subnet, DNS lists contiguous and names valid DNS labels.

e100 and copper e1000 expose auto, 10 half/full and 100 half/full. Other
drivers, and e1000 when STATUS.TBIMODE indicates fiber, expose their current automatic media
only; forced selections return EOPNOTSUPP. Speed/duplex zero mean unknown when
a driver cannot report negotiated media. No forced choice is fabricated.
The UI performs requests on one copied worker job and refreshes explicitly;
acquiring a DHCP lease never blocks that UI turn. Test uses the system resolver
and a two-second, nonblocking TCP/80 connection to the selected source/gateway.
Refusal, timeout and success are distinct results; none implies Internet access.
