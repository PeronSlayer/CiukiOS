# CiukWeb native networking design

## Decision

CiukWeb must not launch `HTGET.EXE` from its Go action: the shell’s external
command route closes the desktop video/assets session before starting a DOS
program. The browser will use a cooperative CAPP network client over a private
INT 61 API owned by the already resident `ICMPD` bridge. `ICMPD` keeps the
physical INT 60 packet-driver receive callback, ARP responder, ICMP responder,
and existing mTCP bridge. A single native client may claim a bounded receive
queue; while claimed, mTCP AccessType is rejected so both consumers cannot
steal the same Ethernet frames. Network parsing and protocol progress happen
from browser poll events, outside the interrupt receive callback.

The first native profile is IPv4 Ethernet, DNS A lookup, TCP, and plain HTTP
on port 80. It is not TLS, IPv6, CSS, JavaScript, or image rendering. The
browser should report HTTPS as unsupported instead of suggesting that it
fetched a secure page. The existing HTML text view remains a bounded document
renderer. HTTP response framing must be explicit and bounded; chunked transfer
coding is part of HTTP/1.1 and must be decoded if the client speaks HTTP/1.1
([RFC 9112, sections 6–7](https://www.rfc-editor.org/rfc/rfc9112.html)).

TCP is a reliable ordered byte stream, so a native implementation needs
sequence/acknowledgment tracking, retransmission, checksums, and connection
close handling; it cannot treat an HTTP response as one packet ([RFC 9293,
sections 2 and 3.8](https://www.rfc-editor.org/rfc/rfc9293.html)). DNS messages
are independently framed UDP queries and replies with compressed names and
resource records ([RFC 1035](https://www.rfc-editor.org/rfc/rfc1035.html)).
ARP resolves the next-hop Ethernet address on the local link ([RFC 826](https://www.rfc-editor.org/rfc/rfc826/)).

## Existing implementation evidence

`src/com/icmpd.asm` already owns an Ethernet wildcard receive callback on the
physical packet driver and forwards non-ARP/ICMP frames to one mTCP callback.
Its receive callback first requests a caller buffer and then receives the
completed packet. The resident timer sends queued ARP/ICMP replies. INT 61
private operations 0–2 report/update the configured IPv4 address and counters.
The new native operations use a fixed claim token and four fixed-size receive
slots; queue overflow is counted and TCP retransmission recovers lost frames.

The mTCP user guide documents HTGet as a DOS HTTP client and requires a packet
driver. mTCP’s TCP/IP implementation is linked into its applications rather
than being a resident system service, which is why launching HTGet cannot meet
the desktop-preserving Go behavior ([mTCP user documentation](https://www.brutman.com/mTCP/download/mTCP_2025-01-10.pdf),
[maintainer explanation](https://brutman.com/forums/viewtopic.php?t=966)).
This implementation reuses the physical packet driver through the resident
bridge; it does not reuse mTCP’s application code or claim mTCP compatibility
for the new native TCP stack.

## Bounded behavior

The INT 61 native API uses a 1600-byte maximum frame, a four-slot receive
queue, exclusive claim/release, a status snapshot (IPv4 address, MAC, queue
depth and drop count), bounded frame send, and bounded frame poll-copy. The
browser advances DNS, ARP, TCP, and HTTP in `EV_POLL`; each invocation drains
at most four received frames and sends only bounded protocol work. A timeout
or queue overflow produces a visible error and leaves the desktop running. No
network parsing or protocol work runs in the packet-driver interrupt callback;
it only classifies and copies bounded frames.

CiukWeb keeps the desktop open while fetching. Its address bar supports
editing, Home/End and caret movement; Go, Back, Forward, Reload and Stop have
separate actions. Escape stops a request, then remains available to the
window’s close handling. Same-origin relative links resolve against the
current URL, and the wheel scrolls three text rows per notch. The renderer
shows the page title when present, an HTTP status for error responses, and a
bounded text view; it does not claim support for HTTPS, scripts, stylesheets,
or images. Successful fetches emit `[CIUKWEB] HTTP status=… bytes=…` and then
`[CIUKWEB] rendered <url>` after response parsing.

## Runtime evidence

The root-owned Linux QEMU/KVM runs `runtime-2` and `ui-3` used the configured
NE2000 PCI NAT device and automatic network startup. CiukWeb resolved
`example.com`, received HTTP 200 with 577 response-body bytes, displayed the
Example Domain page with full wrapped text, and completed a Ctrl+R reload.
The desktop stayed active during the requests; the browser path emitted no DOS
launch markers. Captured screenshots and logs are under
`build/tests/release-0.8.3-2026-10-04/runtime-2/` and
`build/tests/release-0.8.3-2026-10-04/ui-3/`.

This verifies the tested QEMU/KVM NAT profile only. HTTPS/TLS, IPv6, CSS,
JavaScript, and image rendering remain unsupported and are reported as such;
the run does not establish compatibility with other network adapters or
Internet environments.
