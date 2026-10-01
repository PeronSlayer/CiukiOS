# Network settings and adapter management

The Network applet in Control Panel opens from the top-bar network indicator.
It has three sections: IPv4 profile, advanced options, and the detected
network adapter.

## Configuration

The IPv4 section edits address, subnet mask, gateway and DNS server. Advanced
options edit the mTCP host name and Ethernet MTU (576–1500). Apply validates
all fields, including contiguous subnet masks and a host name made of 1–32
letters, numbers or hyphens. It replaces `C:\NET\MTCP.CFG` through a
temporary file and backup, preserving unrelated mTCP options. The host name
also updates `HOSTNAME_ASSIGNED` so IPCONFIG and the editable field agree.
New programs read the saved profile; Ciuki's resident service receives the
new IPv4 address immediately when it is running. Existing clients may need to
be restarted to read other changed values.

The DHCP button runs `NETCFG DHCP`. If the resident network service is not
running, NETCFG first starts `NETSTART`; the mTCP DHCP client then obtains and
saves a lease, and Ciuki's resident address is reloaded. This command
temporarily shows the DOS console and returns to the desktop. It requires a
working packet driver and reachable DHCP server; without those, mTCP reports
an error or times out without creating a lease.

## Adapter management

Rescan reads PCI network controllers and the packet-driver API. The panel
shows the first PCI network adapter's name, vendor/device ID, IRQ and I/O
base when available. A separate line shows whether a physical packet driver
is installed and reads its MAC address. Device Manager opens with the network
adapter selected; Drivers opens at installed network drivers, where users can
enable, disable or install a driver. Those driver changes take effect after a
restart.

PCI hardware detection and packet-driver presence are separate observations.
The packet-driver API does not provide a common cable/link-state query, so
the panel does not claim that an Ethernet cable or remote network is active.
Device Manager lists other adapters when more than one is fitted.

## QEMU evidence

`python3 scripts/qemu_test_network_advanced.py --image
build/full/ciukios-full.img --output build/tests/network-advanced-final-20261001`
checks invalid MTU rejection, static profile persistence, adapter and driver
navigation, and a DHCP lease using a QEMU NE2000 PCI device. Its report and
unedited screenshots are under `build/tests/network-advanced-final-20261001/`.
See [validation record](validation/2026-10-01-network-advanced/README.md).
