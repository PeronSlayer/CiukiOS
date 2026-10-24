# G26 network user tools

Written after `date -Is`: 2026-10-23T23:16:42+02:00.

The binding scope is G26 in `os-feature-inventory.md`. This extends the
existing lwIP/native socket and NetSurf ports; it does not replace their
ownership, memory admission, TLS verification or cancellation rules.

## Reference behaviour and implementation decisions

The owner's Windows 98 Resource Kit, chapter 15, **TCP/IP Utilities**, table
15.6 (`wrkc15_3.htm`, lines 1364–1415), describes ipconfig configuration,
netstat connections/statistics, ping reachability and tracert route discovery.
Appendix F **Setting Up HOSTS Files** (`wrkappf_1.htm`) specifies BSD-style
address/name mappings used by TCP/IP programs, distinct from NetBIOS LMHOSTS.
Chapter 20 (`wrkc20_3.htm`) and Control Panel table 23.1 (`wrkc23_2.htm`)
describe browser options and the Internet applet. This private text is read
in place, not copied into the repository.

* [RFC 792](https://www.rfc-editor.org/rfc/rfc792.html): echo request/reply
  identifiers and sequences, checksum, TTL exceeded and unreachable messages
  including the original IP header and eight payload bytes. Diagnostics must
  match those fields, ignore unrelated traffic, use monotonic deadlines and
  keep pumping Terminal input. Tracert increases TTL and reports missing hops.
* [lwIP sequential API](https://www.nongnu.org/lwip/2_1_x/group__netconn.html)
  and the pinned 2.2.1 `api_lib.c`/`api_msg.c`: enable ICMP raw netconns,
  keep all PCB access on the existing core worker. Reuse the existing bounded
  socket and datagram pools; send ICMP without an IP header and receive it
  with its IPv4 header. IP_TTL is an integer in 1–255. No new panic paths.
* [Microsoft raw sockets](https://learn.microsoft.com/en-us/windows/win32/winsock/tcp-ip-raw-sockets-2)
  documents the same IPv4 receive-header convention. Only ICMP raw sockets
  are added; arbitrary raw protocols, IP_HDRINCL and packet capture remain
  unsupported and must fail explicitly.
* [WSAStartup](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-wsastartup),
  [select](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select),
  [setsockopt](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-setsockopt):
  negotiate 1.1/2.2, preserve Winsock error numbers and thread-local last error,
  translate counted fd sets, socket constants and millisecond timeouts rather
  than exposing native POSIX layouts. Both DLL names must share socket state.
* [NetSurf guide](https://www.netsurf-browser.org/documentation/guide),
  **Download Window** and **Link / New window**, plus pinned 3.11
  `include/netsurf/download.h` and `desktop/download.c`: stream downloads
  into an exclusive temporary file, retain destination choice until Save,
  preserve existing files and remove incomplete data on error/cancel/exit.
  The frontend owns the download context until done/error and must destroy it.
  New native windows can use independent browser processes, preserving the
  existing one-surface-per-process resource accounting.

HOSTS is `/system/settings/HOSTS`: IPv4 address, canonical name and aliases,
ASCII case-insensitive, whitespace and `#` comments, CRLF accepted. Scan at
most 64 KiB, skip malformed/overlong lines, consult before DNS, reopen for
each query so edits take effect. NetBIOS/LMHOSTS remains a separate gap.
Tests must cover malformed input, ownership, errors and cleanup, not just
successful calls. Host evidence does not qualify hardware or Internet sites.

## G26 implementation boundaries

Native raw ICMP uses four raw PCBs and the existing 16-slot datagram budget;
TCP and UDP capacities are unchanged. The i686 production compile measures
network memory and enforces the existing 4 MiB ceiling (`lwip/run.py --kernel`).
No per-process or system memory ceiling is increased.

PNG uses pinned libpng 1.6.59 with custom allocators charged to the image
ledger, strict stream reads and dimension/chunk limits. JPEG uses only the
JPEG half of pinned stb_image 2.30, with the same allocators and static
symbols. stb's PNG decoder was rejected for this frontend because its i686
parse frame exceeds 4 KiB; no warning is disabled. Both output and temporary
allocations are charged. The bitmap conversion follows NetSurf's `bmp.c`.
The [upstream stb source](https://github.com/nothings/stb/blob/f58f558c120e9b32c217290b80bad1a0729fbb2c/stb_image.h)
and [libpng manual](http://www.libpng.org/pub/png/libpng-manual.txt) describe
the memory decode and custom allocation contracts. No new source is vendored.

New Window/Ctrl+N asks the desktop to launch another browser process through
v3 DESK_LAUNCH's URL-only payload (uint32 length + UTF-8 URL, maximum
DESK_ARG_MAX). Only the launcher's existing NetSurf application identity may
send it, the desktop fixes the executable, and the existing launcher reports
late failures. This request reuses the existing opcode and window records. Link target
routing to `_blank` remains separate from this explicit New Window command.

The Internet page writes `/system/settings/internet.cfg` with a temporary
file, fsync and rename. New browser processes apply home, memory cache and
HTTP proxy options. The native fetcher connects to the configured HTTP proxy
and sends absolute request targets, retaining the origin Host header and
cookies, as specified by [RFC 9112, section 3.2.2](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2.2).
HTTPS remains direct with the existing certificate verification;
CONNECT and proxy authentication are not implemented.

The existing privileged NETCTL call also protects GET, so it cannot implement
Terminal's read-only ipconfig/netstat. Add syscall 99 (the next number after
this worktree's highest allocation, FILE_INFO=98): NET_INFO CONFIG copies a
read-only adapter record, SOCKETS copies the bounded cached native socket
list. It never changes interfaces or exposes kernel pointers. Socket state
and endpoint caches are copied under the existing network protection; no
foreign-thread PCB dereference is needed. The descriptor worker maintains
these caches after every operation. NETCTL release/renew retain their existing
capability requirement. The lead must reconcile syscall numbers at integration.

LMHOSTS, automatic link-target windows, browser printing and
JavaScript are not supplied here. Winsock provides synchronous IPv4 calls;
final WSACleanup does not cancel calls retained through temporary fd copies,
and asynchronous/overlapped I/O, FIONREAD and general reverse DNS remain gaps.
The native API truncates datagrams without a separate truncation indicator;
the Winsock bridge consequently cannot yet report WSAEMSGSIZE for short
receive buffers. Resolver results occupy at most 64 per-thread slots between
startup and final cleanup; final cleanup reclaims all slots, but thread exit
does not yet reclaim them during a long-lived startup session. Browser processes use unique profile staging files, but
their in-memory history/bookmark models do not yet merge concurrent edits.
These are compatibility gaps, not completed Windows 98 behaviour.

## Integration with the desktop feature branches

The integration preserves the existing input-settings protocol, Users, Regional
Settings and Multimedia applets. Internet is appended as CP_INTERNET=19
(CP_PAGES=20), so existing page values 0–18 keep their meaning. NET_INFO=99
does not collide with the integrated syscall table; DESK_LAUNCH reuses its
existing opcode. Browser download polling and cleanup run alongside G31
printing polling and cleanup, with both clipboard retention and PNG/zlib
link flags. The G26 raster handlers register after upstream image handlers
so upstream registration cannot replace their allocation ledger; the other
fx-24 handlers keep their registrations. G31 visible-page printing and fx-24 image formats remain available.
The [NetSurf guide](https://www.netsurf-browser.org/documentation/guide)
documents separate download and new-window operations; the existing
[RFC 9112 absolute-form rule](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2.2)
continues to govern the unchanged HTTP proxy path. These references were
checked during integration; no new network behaviour is introduced here.
