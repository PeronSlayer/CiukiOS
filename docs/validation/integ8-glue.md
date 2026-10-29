# Integration 8 glue

Research and implementation decisions for `docs/directives/integ8-glue.md`.

- Wine 10.0's [dplayx entry point](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/dplayx/dplayx_main.c)
  calls [DPLAYX_ConstructData](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/dplayx/dplayx_global.c).
  The pinned local upstream source in `build/external/wine-10.0` confirms that
  construction creates a semaphore and pagefile shared mapping, initializes
  lobby data, and opens no network endpoint. None of patches 0011–0015 changes
  DllMain or the constructor. The f6-26 merge (`a63af1fb4`, carried by lan2)
  added this DLL to the reused set; f6-28 changes suite wiring, not DllMain.
  H1's default transport refuses these object/mapping records with ENOSYS.
  Supply bounded, owned semaphore/shared-memory services in the host harness;
  preserve the production DLL, provider initialization and recorded PE hashes.
  The upstream web pages could not be fetched in this environment; the pinned
  release source was inspected directly instead.
- Microsoft's [DllMain documentation](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain)
  permits simple synchronization-object initialization and describes attach
  failure/unload behavior. Keep transport/network initialization out of attach;
  do not bypass constructor failures or make DllMain unconditionally succeed.
- Microsoft's [CreateSemaphore](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createsemaphorew)
  specifies case-sensitive names, existing-object status and count consumption
  by waits. [CreateFileMapping](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw)
  describes pagefile mappings; [UnmapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile)
  describes view lifetime. H1 supplies only the constructor's single-process
  subset: eight local objects, sections up to 1 MiB and one whole-section view,
  retaining storage until both handles and view are gone. Unsupported services
  and blocking waits remain ENOSYS; installer/game service routing is unchanged.
- Microsoft's [service timeout documentation](https://learn.microsoft.com/en-us/troubleshoot/windows-server/system-management-components/service-not-start-events-7000-7011-time-out-error)
  describes ServicesPipeTimeout in milliseconds. K8-8 is the lead's explicit
  decision to allow 30,000 ms for CiukiOS session readiness; this is a CiukiOS
  launch policy, not a claim that Windows shell launch has that deadline.
  Test the old deadline, the final millisecond before expiry, and exact expiry
  against the production security poll path. Leave the separate 5,000 ms service
  reassembly deadline unchanged.
- The acc-t23 additions use `id` prefix `acc-install-` (20 installer/sound-setup
  cases) and `guests` (LAN pairs); all extras are at most 2,400 seconds.
  Extend the security suite's stale extra-case filter without changing bounds.
- IPX frame configuration is a machine setting: use the existing administrator
  policy and map the census to `system_services_test.c::test_settings`.

Commands, measured results and remaining lead validation are recorded in
`build/codex/integ8-report.md`.
