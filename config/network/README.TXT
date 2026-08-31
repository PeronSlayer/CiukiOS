CiukiOS network share

Default QEMU configuration:
  guest IPv4: 10.0.2.15/24
  gateway:    10.0.2.2
  physical NIC: INT 60h
  mTCP API:     INT 61h
  FTP user:   ciukios
  FTP pass:   ciukios
  FTP root:   C:\SHARE
  host FTP:   127.0.0.1:8021
  host PASV:  127.0.0.1:2048-2303

Start networking from any directory:
  NETSTART
  IPCONFIG
  PING 1.1.1.1
  FTPSRV

Configure and persist IP, subnet, gateway and DNS:
  NETCFG STATIC 10.0.2.15 255.255.255.0 10.0.2.2 1.1.1.1
  NETCFG DHCP

NETSTART loads NE2000 and the permanent ARP/ICMP responder. Incoming ping
keeps working when FTPSRV is stopped. IPCONFIG reports its live status.
QEMU user NAT supports outbound Internet ping but not direct host-to-guest
ping. On Linux use ciukios_tap.sh up-nat plus the --tap runner option.

FTP is unencrypted. Use the default credentials only behind QEMU NAT or on
a trusted isolated LAN. Change FTPPASS.TXT before bridged/physical use.
