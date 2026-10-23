# f6-07 — NDIS host step 4: Device Manager "Install driver from pack" flow

- **Phase/model:** F6, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f6-07` from `wt/f6-05`.
- **Contract:** `docs/design/ndis-miniport-host-contract.md`, ND11 and the f6-07 row of ND13. The owner's pack stays outside Git, on the owner's disk or a CD; no payload is redistributed.

## Required

1. **Device Manager action "Install driver from pack…"** on a network adapter.
   - **Selection:** pick the pack location (a folder or a CD), parse INFs with the existing `scripts/build_hwids.py` rules ported to the target, and select by exact hardware id, OS decoration (NT.5.x) and service.
   - **Preview:** show driver name, version, provider and files, plus whether the binary has a lead qualification record. Unqualified drivers install as disabled with that reason.
2. **Transactional install.** Copy the `.sys` and support files to `/System/drivers/<inf-dir>/`, write typed parameters (`AddReg`, `Includes`/`Needs`, defaults) into the driver's registry key, and write a manifest with hashes. Roll back on any failure. Uninstall and rollback are supported, with no ledger growth after repeated install and remove.
3. **Binding.** At boot the NDIS host loads only installed, qualified manifests for present devices, and the native driver stays the default (ND2).
4. **Tests:** synthetic INF and package fixtures (never the owner's pack), selection precedence, typed parameters, rollback, uninstall, and the Device Manager dialog goldens.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/f6-07-report.md`.
