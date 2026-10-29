# integ9-glue: the two remaining integration-9 suite failures

- **Written:** 2026-10-29 05:59 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-integ9`, branch `wt/integ9` at `05020767b`. The canonical build is green (image `1f36fbcf`), and so are the kernel suite, the desktop suite, the smoke and three qemu-t23 first boots.
- **Evidence:** the lead's logs `build/lead-scratch/chain-integ9/win32.log` and `security.log` in the main checkout.

## Scope

1. **Census (K8-6).** `test_policy_coverage` reports `devctl: COM_CLEAR_ERRORS has no policy/test mapping`. fx-hlutils added it for `ClearCommError`.
   - Add its row in `tests/host/security/policy_census.py` with the same policy as the other serial-port device controls.
   - Map it to the fx-hlutils test that exercises it.
2. **wininet attach.** `test_wine_upper.LoadTests.test_h1_dllmain_attach_detach_of_each_reused_dll` fails for `wininet.dll` with `dllmain-attach-failed`, `attach_false=1` and error 1114.
   - In Wine 10.0, `wininet` `DllMain` allocates a TLS index and runs `init_urlcache`. That creates two events and initialises the cache containers from the user shell folders in the registry ([dlls/wininet/internet.c](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/wininet/internet.c), [urlcache.c](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/wininet/urlcache.c)).
   - Find which call fails in H1 and say whether it is a harness gap or a production gap. integ8-glue already added bounded semaphore and section records to `upper_h1.c` for dplayx; extend that pattern for a harness gap.
   - For a production gap (an event, a registry value or a folder the CiukiOS runtime does not provide), fix the runtime and add a host test. `wininet` must attach in a real CiukiOS session.
3. Run sequentially with `nice -n 19`:
   - `tests.host.win32.wine.test_wine_upper`;
   - `tests.host.security.test_policy_coverage`;
   - the fx-hlutils comm tests;
   - if production code changed, the win32 tests that cover it.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/integ9-report.md`. For each item, say whether it is a harness or a production fix.
