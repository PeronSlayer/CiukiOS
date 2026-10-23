# f6-06 — NDIS host step 3: ndis.sys host in the user-mode driver host, netif adapter, wait bridge, STI shadow

- **Phase/model:** F6, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f6-06` from `wt/f6-05`.
- **Contracts:** `docs/design/ndis-miniport-host-contract.md`, ND9–ND13 and ND14/ND14.1, plus `docs/design/ndis-loader-boundary.md`.
- **Lead decisions (2026-10-23 01:47)**** on the f6-05 report:
  1. **STI shadow.** Model the architectural one-instruction STI shadow. After an emulated STI, delivery of a pending virtual interrupt waits until the next instruction retires; use single-step (TF) for that instruction. Test it with sti/hlt, sti/ret and sti/pushf patterns.
  2. **Wait bridge.** Implement the asynchronous user-continuation wait bridge for NdisWaitEvent and the KeWaitFor\* paths inside foreign callbacks. The waiting callback yields its host thread back to the dispatcher, ISR/DPC delivery continues, and the wait resumes on signal or timeout with exact NDIS semantics. Never fabricate success or timeout.
  3. **Qualification stays lead-owned**, through QEMU `e1000` and `pcnet` (allowed for F6) and the T23 e100. Execution opens per profile only through a qualification record the lead adds.

## Required

1. **ndis.sys host.** Implement inside the host process the NDIS 5.0/5.1 export union of the three pinned profiles: registration and characteristics, MiniportInitialize, attributes, shared memory and map registers, interrupt registration, send and receive packets, OID query and set, reset, halt and CheckForHang. Use the macro and structure ABI with exact layouts, and the packet and buffer lifetimes of ND10.
2. **netif adapter (ND9).** Publish the miniport as a `netif_driver` through the validated shared ring of ND14.1. The kernel copies accepted frames into its own storage. Link state goes to f2-100's netctl records.
3. **Tests.** Synthetic miniports (no pack binaries) exercise initialization, IRQ/DPC with the wait bridge, serialized and deserialized sends, receive indication, OIDs, reset, halt and teardown under fault, plus the STI shadow cases. Run the NDIS, kernel and ABI suites; LSan exits in the sandbox are reported as such.
4. **Lead hooks.** Provide the probe commands and records the lead needs to qualify `e1000325.sys` on QEMU `e1000` and `atnicm51.sys` on `pcnet`: the exact hashes from the pack, and the expected record sequence.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (NDIS 5.1 DDK pages). Host tests one at a time with `nice -n 19`. Write `build/codex/f6-06-report.md`.

## Amendment 1 (lead, 2026-10-23 03:51): the four refused exports

Implement `NdisMRegisterDevice`, `NdisMDeregisterDevice`, `NdisMSetMiniportSecondary` and `NdisMPromoteMiniport` as real exports with honest results; never fake success.
- **`NdisMRegisterDevice`** returns `NDIS_STATUS_NOT_SUPPORTED` (no control device is created; the host serves no user-mode I/O to a miniport) and records `[ndis] control-device refused driver=…`.
- **`NdisMDeregisterDevice`** returns `NDIS_STATUS_FAILURE` for any handle, since none can exist.
- **`NdisMSetMiniportSecondary` and `NdisMPromoteMiniport`** return `NDIS_STATUS_NOT_SUPPORTED`, because a host serves exactly one adapter and teaming is unsupported.

The four are documented in the export table as supported-with-refusal, and the three pinned drivers then pass import resolution. Tests: import resolution of the three exact import sets (from the census), each refusal path, and a synthetic miniport that treats a refused control device as optional and continues initialization.
