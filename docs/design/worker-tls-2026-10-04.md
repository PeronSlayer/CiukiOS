# Preemptible HTTPS worker

The browser's HTTPS implementation runs in the existing VMFORK/DPMIRUN DOS
worker, not in the 16-bit CAPP or the CVSESSION ring-0 device handlers. The
desktop keeps the TCP connection and sends/receives encrypted bytes through
the versioned XMS mailbox in `src/web/worker_abi.h`; the worker owns the TLS
state and returns plaintext. Calls are bounded request/response operations,
so an RSA certificate check may take time without disabling interrupts or
blocking the desktop scheduler.

Use BearSSL 0.6 for TLS 1.2. Its engine is a push/pull state machine with
caller-owned buffers and no required allocator. Configure a strict TLS 1.2
version range, DNS SNI and hostname validation, explicit trust anchors, and
the current UTC time. Fail the connection if any of those inputs is
unavailable or certificate validation fails. Do not retry as cleartext HTTP.
BearSSL's MIT license and upstream copyright notices must remain with the
vendored source.

Seed the TLS engine only from the worker's virtio-rng source. The RNG service
must fail closed if the PCI device, queue, or bounded wait is unavailable;
timestamps, counters, and unseeded PRNG output are not entropy. The current
worker RNG maps the DMA span with a bounded LDT selector and copies bytes via
the selector-aware move path; it does not treat the physical mapping as a near
pointer relative to global DS. QEMU's transitional `virtio-rng-pci` is a
supported virtual source; physical machines need an equivalent OS-provided
cryptographic random source before HTTPS can be enabled there. PCI device
ownership and handoff details are documented separately. Use RTC-derived UTC
for X.509 validity checks only when the desktop reports a valid clock; a
missing/invalid clock fails closed.

The 128 KiB locked-XMS allocation has a 64 KiB mailbox followed by a 64 KiB
virtio DMA area. The DPMI worker maps the mailbox for CPU access and points the
legacy virtio queue only into the DMA area. The mailbox mapping was
runtime-checked twice through worker spawn, physical map, exact echo, kill,
reopen, and second echo. This shared memory is data only; no network pointer or
worker pointer crosses the ABI. The trust bundle carries the complete upstream
Mozilla Public License 2.0 text in `third_party/cacert/MPL-2.0.txt`. Worker-owned
code dispatches `TLS_OPEN`, `TLS_STEP`, `TLS_WRITE`, and `TLS_CLOSE` operations
and copies bounded input/output data. Its BearSSL X.509 vtable is initialized
through the wrapper's supported initialization path. The TLS wrapper itself
knows nothing about the mailbox or the TCP stack.

BearSSL 0.6's minimal X.509 verifier matches the configured hostname against
SAN `dNSName` values (or the Common Name only when SAN is absent), but it does
not perform the required binary comparison against SAN `iPAddress`. For an
IPv4 literal, the worker therefore omits DNS SNI, delegates time/chain/signature
validation to BearSSL with name matching disabled, and additionally requires
an exact four-byte IPv4 SAN match in the leaf certificate. Its small strict-DER
parser captures only the first certificate and fails closed if it exceeds
32 KiB or the SAN is absent, malformed, or mismatched. DNS hostnames continue
through BearSSL's normal hostname verification.

For HTTP over TLS, a completely received `Content-Length` body or terminal
chunk is considered complete after pending TLS plaintext is drained, even if
the TCP peer closes without `close_notify`. The client still sends its own
TLS `close_notify`, waits for the alert record's TCP ACK, and then sends TCP
FIN; it does not wait for a peer close alert after the complete HTTP frame.
An EOF-delimited body still needs the authenticated TLS close alert; a bare
TCP FIN must not define its end. This follows RFC 2818 §2.2.1: a fully framed
body may complete when the connection closes early, while missing framed data
remains a truncation error.

Sources:

- [BearSSL project and download](https://bearssl.org/): C implementation,
  compact embedded target, version 0.6, MIT license, released source archive.
- [BearSSL API overview](https://bearssl.org/api1.html): streamed
  state-machine interface; I/O remains outside the TLS engine.
- [BearSSL SSL API](https://bearssl.org/apidoc/bearssl__ssl_8h.html): engine
  state flags, record/application buffers, explicit TLS-version range, and
  entropy injection contract.
- [BearSSL X.509 notes](https://bearssl.org/x509.html): explicit trust
  anchors, certificate validity time, and server-name checks.
- [RFC 5280](https://www.rfc-editor.org/rfc/rfc5280.html): IPv4 `iPAddress`
  SANs are four network-order octets; DNS names use the separate `dNSName`
  GeneralName type.
- [BearSSL RNG API](https://bearssl.org/apidoc/bearssl__rand_8h.html): a
  seeder must supply at least 128 bits of entropy or report failure.
- [RFC 8446](https://www.rfc-editor.org/rfc/rfc8446.html): secure random
  requirement for TLS ClientHello values.
- [RFC 2818 §2.2.1](https://www.rfc-editor.org/rfc/rfc2818.html): HTTPS
  message framing and TCP closure before `close_notify`.
- [QEMU invocation reference](https://www.qemu.org/docs/master/system/invocation.html):
  `virtio-rng` uses QEMU's built-in entropy backend by default; the default
  `virtio-rng-pci` legacy PCI device ID is `1af4:1005` ([QEMU PCI ID table](https://www.qemu.org/docs/master/specs/pci-ids.html)).
- [DPMI 1.0 specification](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf):
  physical-address mappings are a DPMI capability; the bundled HDPMI/Jemm
  mailbox mapping and XMS-sharing path passed the bounded worker runtime check.
  The RNG DMA mapping uses a bounded LDT selector and selector-aware data
  movement, with the full queue span checked against the selector limit.
- [XMS 3.0 specification](https://ps-2.kev009.com/basil.holloway/ALL%20PDF/Microsoft_XMS_3%5B1%5D.0_Specification.pdf):
  locking an XMS block pins it and returns its address for the lock lifetime.

The pinned Mozilla-derived trust bundle is `third_party/cacert/CACERT.PEM`; its
provenance, included license text and digest are recorded beside it. Six
IPv4-SAN parser checks and 101 WebNet host checks passed under AddressSanitizer
and UndefinedBehaviorSanitizer. These host checks do not establish a complete
TLS handshake or QEMU runtime pass. Full VM validation of the RNG device,
trusted and rejected certificates, and HTTP framing remains pending; no
physical-hardware entropy or broader HTTPS compatibility claim is made yet.
