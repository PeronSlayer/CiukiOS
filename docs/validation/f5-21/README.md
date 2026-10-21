# F5-21 probe registration and fixture observations

Research checked on 2026-10-20, before continuing WIP `4c7627b1`.

- [Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc): reservation addresses round to allocation granularity, committed memory is zero-filled, and occupied ranges cannot be re-reserved. Keep the existing 64 KiB-aligned dialogs page at `0x10000000`; upper and kernel32 use `0x10010000` and `0x10020000` in their separate processes. The repository's `tests/host/win32/kernel_loader_main.c:dialogs_startup` demonstrates that `0x6fff0000` belongs to loader metadata. Extend that production-loader check to each fixture instead of treating the top of the arena as free.
- [Microsoft CreateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread): default creation starts the thread immediately and a successful wait is required before checking its exit status. The kernel32 fixture joins its worker before completing the result block, and checks TLS isolation in both threads.
- [Microsoft GetPrivateProfileStringA](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilestringa): returned length excludes the NUL terminator; a missing key returns the specified default. Keep those exact expectations in the kernel32 fixture.
- [Microsoft DDE reference](https://learn.microsoft.com/en-us/windows/win32/dataxchg/dynamic-data-exchange): raw DDE and DDEML are separate client interfaces. Existing host DDE transcripts alone do not establish a guest end-session/restart observer. Do not publish a guest success from host transcripts or application stdout.

- [QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html): the named fw_cfg file contains bytes supplied at runtime. The 23-byte probe-name bound belongs to CiukiOS's selector buffer, not the fw_cfg transport. Keep short manifest aliases and preserve full workload IDs in scripts and evidence.
- [Microsoft PE/COFF](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format): verify the fixture's PE import descriptors and names against the built store; deterministic rebuilds retain the same PE bytes.

The f5-21 directive names repository probes and cases, with no external URL of its own. These are the primary API references required by its referenced f5-06 and f5-07B directives. Guest results remain unqualified until the lead runs the canonical image. No QEMU or image build is performed here.

Required item 0 explicitly requests the upper-DLL write-gate predicate. Add that assertion to the upper-DLL suite case and to its existing simulated H1 envelope; retain every prior predicate. The default host discovery already contains `test_runner_selectors.py`; package markers also make the directive's explicit `-t .` command importable on this host.
