# integ8-glue: integration-8 suite failures and the session launch deadline

- **Written:** 2026-10-29 00:05 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree and branch:** `/home/peronslayer/Desktop/CiukiOS-wt-integ8`, branch `wt/integ8` at `ecea8aada` (main + lan2 + f7-24 + sec-k08b + fx-hlrun + fx-w32green + fx-games20b + sec-firstboot r1 + fx-games20c + f6-28).
- **Evidence:** the lead's chain logs in `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/chain-integ8/` (`desktop.log`, `win32.log`, `security.log`, `t23-*.log`) and the failing qemu-t23 serial `build/test-runs/f0-smoke/698e4045/serial.log` in the worktree.

## Scope

1. **Security host link.** `tests/host/security/system_services_test.c` no longer links: `sysvc_jobs.c` now calls `net_ipx_config_lock`, `net_ipx_config_unlock`, `net_ipx_frame_publish` and reads `net_ipx_frame_type`. Add the IPX configuration source to that test's build, as `scripts/test/host_floppy_tests.sh` already does with `ipx_config.c`. Check every other host build that compiles `sysvc_jobs.c` for the same gap.
2. **Policy census (K8-6).** `test_policy_coverage` reports `call99-action: CIUKI_SYSTEM_SETTING_IPX_FRAME has no policy/test mapping`. Add its row in `tests/host/security/policy_census.py`. Use the same policy as the other machine network settings: administrator only. Map it to the test that already exercises the setting in `system_services_test.c`'s `test_settings`.
3. **acc-t23 budget.** `test_owner_security_items_in_acc_t23` totals 146,640 s against 100,800 s. Its `extra` filter recognises only cases with `dos_game` or `guests`. Identify which cases fx-games20c and f6-28 added (installer and sound-setup cases, LAN cases) and how they are keyed.
   - If each new case is at most 2,400 s, extend the filter to recognise them by their real keys. Do not raise any constant.
   - If a case exceeds 2,400 s, do not change the bound. Record the case and its timeout in the report, and the lead will decide.
4. **dplayx DllMain.** `test_wine_upper.LoadTests.test_h1_dllmain_attach_detach_of_each_reused_dll` fails for `dplayx.dll`: attach returns 0 and the outcome is `terminated` with exit 90. Find which merged branch changed `dplayx` DllMain (lan2 or f6-28) and why `DLL_PROCESS_ATTACH` fails in the H1 harness.
   - Wine's `dplayx` DllMain only builds its shared data (`DPLAYX_ConstructData`) and never opens a network endpoint. Attach must succeed with no network service. Any IPX or TCP/IP initialisation belongs to the first service-provider call, not to attach.
   - Keep the live LAN behaviour of f6-28 and lan2 unchanged.
   - Update the reused-DLL hash in `config/wine-upper.json` if the build requires it.
5. **Desktop tests.**
   - `test_system_icons.test_image_builder_stages_every_icon_and_notice` and `test_control_thumbnails.test_deterministic_previews_inventory_and_invalidation` patch `image.sha256` to `'fixture'`, so the new Radeon `firmware_payloads()` check fails. Patch `firmware_payloads` in those tests the same way as the other payload groups. Do not weaken the production hash check.
   - `test_desktop.test_build_provenance_and_payloads`: the manifest now lists `sectest` (U6-4, `/Programs/sectest` stays in the image). Add it to the expected set with the decision as its comment.
6. **Session launch deadline (lead decision K8-8 in `docs/directives/sec-lead-decisions.md`).** Raise `CIUKI_SECURITY_LAUNCH_TIMEOUT_MS` from 5000 to 30000. Update every place that carries the value:
   - `src/kernel/include/ciuki/abi.h`;
   - the `abi_layout_test.c` constant row;
   - the constants table in `docs/design/user-security-kernel.md`;
   - `docs/developer-api.md`, through `scripts/gen_api_reference.py`;
   - any host test that advances ticks by the old 5000 ms deadline. Keep a test that proves an unready launch fails with `-ETIMEDOUT` at the new deadline and not before.

   Add a short note under the sec-user03 deadline paragraph in `docs/validation/sec-user03.md` pointing to K8-8. Leave the original sentence as it is.
7. Run sequentially with `nice -n 19`:
   - the security suite (`python3 -B -m unittest discover -s tests/host/security -t .`);
   - the failing win32 test module (`tests.host.win32.wine.test_wine_upper`);
   - the three desktop test modules above;
   - `tests.host.test_acceptance`;
   - `python3 scripts/gen_api_reference.py --check`;
   - the kernel host tests touching the security launch path.

## Rules

- Never commit or stage; the lead commits.
- Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/integ8-glue-report.md`. For each item, give the cause, the change and the test result.
