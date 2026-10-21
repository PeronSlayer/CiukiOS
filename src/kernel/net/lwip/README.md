# lwIP 2.2.1 kernel port (f6-10)

`import.json` pins the offline archive, compiled source list and patch hashes.
`build.py` verifies these before extraction and verifies every cached file before
reuse. There is no downloader or alternate version. The upstream archive and
its unauthenticated signature remain in `build/downloads/lwip/`; extraction is
under `build/external/lwip-2.2.1`. Upstream files are changed only by the recorded
patches, never by editing the cached source. The compiler includes all listed
core/API files; upstream `api/sockets.c` is excluded. This directory's
`sockets.c` implements CiukiOS descriptions and is separate kernel code.

Upstream BSD-3 notices are retained in the extracted sources and `COPYING`.
The combined compiled-source/header notices, archive identity and file list are
also under `docs/licenses/`; the image recipe installs them under
`/system/licenses`. CiukiOS port code is GPL-2.0-only; SDK headers/wrappers and
the shared native ABI remain MIT as required by F2.

The four upstream patches have narrow purposes:

1. Return allocation failure when the TCP PCB pool is exhausted, preserving
   live/TIME_WAIT PCBs instead of using upstream's eviction fallback.
2. Invalidate cached DNS answers and synchronously drain pending callbacks and
   their UDP source PCBs when a DNS server changes. The adapter increments its
   resolver generation before draining; late results cannot populate a new
   generation's cache or release a reused request.
3. Use a correctly typed loopback callback rather than an incompatible function
   pointer cast (checked by the real-stack UBSan harness).
4. Replace the entire DHCP lease DNS list on ACK, clearing omitted server slots
   through the existing generation/cache invalidation hook. This covers
   same-address renewals from two servers to one or no servers.

Amendment 1 (2026-10-21) retains fd identity before user-memory preparation,
checks cancellation/deadlines at queued commitment, and keeps connect results
separate from SO_ERROR. Address loss uses retained endpoint/route state even
after upstream aborts a PCB. Successful full shutdown preserves EOF/HUP without
inventing an error. DHCP startup failures expose an internal error and retry
after one monotonic second. `../lwip_handoff.c` owns copied ingress/output;
`../bridge.c` remains reserved for f6-11's additive DOS tap.

The close setting is 500 ms: pinned api_lib initializes ceil(ms/500)+1 polls,
so this selects two 500 ms polls. A separate raw TCP path converts FIN-allocation
failure to success with TF_CLOSEPEND, bypassing those polls. The persistent
exhaustion fixture records this distinction; the raw-PCB deadline still needs
the lead's decision (see the validation note).

`lwipopts.h` is shared verbatim by target and host. The native 64-bit host
requires pointer-aligned allocator storage in `arch/cc.h`; this changes no pool
counts or protocol options. i686 measurements use the production alignment and
flags. The core owns raw/netif/configuration calls; normal and control workers
have exclusive sequential ownership. Socket and output request storage is
fixed. User waits and cleanup retain request storage through late completion.
Link callbacks use preallocated core messages. The existing driver ARP probes
remain available as driver evidence; normal ingress belongs solely to lwIP.

Focused checks (run serially with `nice -n 19`):

```sh
python3 tests/host/lwip/run.py
python3 tests/host/lwip/run.py --kernel
python3 tests/host/lwip/fd_run.py
python3 tests/host/lwip/fd_run.py --frame
python3 tests/host/lwip/sdk_compile.py --headers build/tools/ciuki-sdk/sysroot/include --run
```

The host tests use real pinned lwIP, pthread synchronization and scripted
Ethernet peers. They do not use Linux sockets as the stack. Sanitizer leak
detection remains enabled; a sandbox denying ptrace can prevent its final
leak scan and must be reported as an unsuccessful sanitized command.
