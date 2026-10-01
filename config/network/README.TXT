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
  HTGET -o C:\SHARE\PAGE.HTM http://example.com/
  FTPSRV

Configure and persist IP, subnet, gateway and DNS:
  NETCFG STATIC 10.0.2.15 255.255.255.0 10.0.2.2 1.1.1.1
  NETCFG DHCP

From the graphical desktop, click the network indicator in the top bar.
Control Panel > Network also edits host name and MTU, shows the detected
adapter and packet driver, and opens Device Manager or installed drivers.
The DHCP button starts NETSTART when needed and returns to the desktop after
the lease attempt. Driver enable/disable changes require a restart.

NETSTART loads NE2000 and the permanent ARP/ICMP responder. Incoming ping
keeps working when FTPSRV is stopped. IPCONFIG reports its live status.
QEMU user NAT supports outbound Internet ping but not direct host-to-guest
ping. On Linux use ciukios_tap.sh up-nat plus the --tap runner option.

After NETSTART, return to the desktop and open Application Library >
Applications > CiukWeb. Enter an HTTP URL and select Go. CiukWeb is a
native desktop module; it uses C:\NET\HTGET.EXE for HTTP transfer.
It displays simple text and absolute HTTP links. HTTPS, images, JavaScript
and CSS are not supported. MicroWeb is no longer preinstalled.

FTP is unencrypted. Use the default credentials only behind QEMU NAT or on
a trusted isolated LAN. Change FTPPASS.TXT before bridged/physical use.
