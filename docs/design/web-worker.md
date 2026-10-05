# Preemptible web worker

The owner requested HTTPS, CSS and JavaScript on 2026-10-04 and reported that
browser work delayed desktop input. The CAPP browser approaches its 64 KiB
code/data limit. Cryptography and JavaScript therefore run in a separate
32-bit user-mode process, started with the existing VMFORK/DPMIRUN path.
They must not run in CVSESSION's interrupt-disabled ring-0 device handler.

The desktop owns a 128 KiB XMS block, locks it before spawning the worker,
and communicates through a versioned request/response mailbox. Only one
request may be outstanding. A sequence number publishes completed input;
a matching response number publishes completed output. Lengths, operations
and status are checked on both sides. Cancellation ends the worker before
unlocking or freeing its mailbox. The upper 64 KiB is reserved for the RNG
DMA queue and entropy buffer; it never overlaps message payloads.

Normal worker shutdown is cooperative. The owner first waits for any published
request to complete, then publishes `CWW_EXIT`. The worker releases its RNG and
certificate state, frees its DPMI selector and physical mapping, and exits
through DPMIRUN/VMFORK. The owner retains the locked XMS mailbox while the VM is
alive and retries close asynchronously; after a two-second grace period it may
use `VMM_KILL` if the worker is still present, and still waits for VM teardown
before releasing the mailbox. This shutdown path is implemented but awaits
runtime validation.

The idle loop calls DJGPP `__dpmi_yield()`, which issues INT 2Fh/AX=1680h.
DPMI 1.0 defines that call as releasing the current VM's time slice. CiukiDOS
currently returns AL=0 for 1680h without switching, so `/B` DPMIRUN now hooks
that vector only for its background mode and translates 1680h into the existing
V86 `STI; HLT` callback; other multiplex calls chain to the prior handler. The
hook is restored during VM teardown. This lets the existing scheduler switch
to a ready VM immediately instead of waiting for the normal two-tick quantum.
Protected-mode reflection to the per-VM vector and the HLT switch still need
runtime validation.

Research before implementation:

- [DJGPP physical mapping guidance](https://delorie.com/djgpp/v2faq/faq18_7.html):
  DPMI 0800h maps physical memory above 1 MiB, followed by a bounded selector.
- [DJGPP descriptor allocation](https://delorie.com/djgpp/doc/libc/libc_202.html):
  the worker uses the DPMI API rather than assuming DS has a physical base.
- [DPMI 1.0 specification, INT 2Fh function 1680h](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf):
  defines the current-VM time-slice release request and its AL=0 supported
  return value.
- [BearSSL engine API](https://bearssl.org/api1.html): record and application
  data use a push/pull state machine. Certificates, hostname, trust anchors,
  time and cryptographic entropy remain mandatory.
- [MicroQuickJS](https://github.com/bellard/mquickjs): embedded ES5-oriented
  engine with an explicit memory arena and interrupt callback.
- [CSS 2.1 cascade](https://www.w3.org/TR/CSS21/cascade.html): style selection
  requires specificity, source order and inheritance, not keyword replacement.

Repository evidence: `dosvm.c:spawn` already starts VMFORK from a CAPP through
DOS EXEC. `session_vmm.inc` virtualizes conventional memory while extended
memory stays global. The locked XMS mailbox uses that shared physical memory;
worker virtual addresses and conventional buffers are never used as DMA
addresses. The mapping and cancellation contract must be validated in the
full Linux VM before claiming HTTPS or JavaScript support.

The first full Linux VM transport proof passed on 2026-10-04: the 16-bit CAPP allocated
and locked the XMS mailbox, started a DJGPP worker under `DPMIRUN /B`, and
received an exact byte-for-byte response through DPMI's physical mapping.
That run used the earlier force-kill teardown; it proves mailbox transport,
safe release after the VM ended, and reopen, but does not validate the newer
graceful `CWW_EXIT` path. Evidence is under
`build/tests/desktop-web-audio-2026-10-04/worker-proof/`. TLS, script behavior,
and graceful shutdown require their own integration checks. Background mode
omits guest audio/input devices.
