# f9-04b — Setup native integration: kernel interfaces, Setup-mode authority, manifest, real libsetup in the wizard

- **Phase/model:** F9, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f9-04b`, from `wt/f9-04` with `wt/f9-03` merged in.
- **Owner (2026-10-22):** a CiukiOS distribution that anyone can install on their own retro PC. The Setup CD must really install.
- **Sources:** the decisions of the f9-03 report (items 2–5) and the f9-04 report (items 1–8), and `docs/design/cd-setup-plan.md`.

## Lead decisions (2026-10-23 01:05), binding

1. **VFS interface.** Add a bounded, generation-safe native query, allowed only to the process holding `setup_capability` in Setup mode, with ABI oracle rows and the API reference regenerated:
   - the read-only FAT cluster map of a file;
   - full FAT attribute read and write;
   - the 8.3 alias query.
2. **Configuration writers.** Setup writes the target system's configuration through the same writers the Control Panel uses, made destination-aware (target volume prefix). That covers identity registry keys, time zone and the RTC (local time versus UTC per the existing clock setting), the network configuration in the f2-100 persistence format (DHCP, static or none), and bundled-game registration. IPX/lobby persistence waits for its own feature.
3. **Manifest.** The boot manifest gains the directory set, numeric FAT attributes, and an exact canonical BOOT.CFG template with its SHA-256. Setup writes BOOT.CFG from that template and its own options, byte-identical; it does not reconstruct arbitrary originals.
4. **Loader source.** Reading the verified loader bytes from offset 512 of `D:/CIUKBOOT.IMG` is approved.
5. **Media identity.** `setup_plan.source_iso_sha` is the SHA-256 of the embedded `CDMANIFE.JSN`, the media identity. The final ISO hash lives in the external manifest. Rename the field's documentation accordingly; the layout is unchanged.
6. **Setup-mode authority.** In Setup mode the supervisor installs the display and input grants for the Setup process and allows its POWER restart and shutdown requests, through `setup_capability`. Every other process stays unchanged.
7. **Interface amendment.**
   - Add `erased_total` to the summary, so the preview shows 16 records plus "and N more" instead of refusing.
   - `required_bytes` is libdisk's inclusive total, with payload, overhead and reserve as breakdown fields.
   - Version the interface and record the change in the plan.
8. **Errors.** INTERNAL+EINVAL/ENOTSUP stay for now.

## Required

Implement decisions 1–7 in the kernel, the SDK, `scripts/build_image.py`/`build_cd.py` and libsetup. Connect the f9-03 wizard to the real libsetup through the native adapters, replacing the fake backend in the production build and keeping the fake for host tests. Run the host tests of f9-01–f9-04 plus new tests for each decision. Native prepare and execute must work end-to-end on a file-backed disk in the host model, and no write may happen before the user confirms.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f9-04b-report.md`.
