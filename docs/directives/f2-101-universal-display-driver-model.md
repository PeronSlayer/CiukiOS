# f2-101 — Universal display driver model: registry, GDI-style commands, software engine (part A)

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f2-101` from `main`.
- **Owner decisions (2026-10-22 ~23:10):** CiukiOS must be universal, not specific to the T23; remove every Savage-specific reference from generic code; GDI-style accelerated drawing with a software fallback first, native driver families next.
- **Contract:** `docs/design/display-driver-model.md` (DM1–DM13), binding. Read it completely, including any lead amendments.

## Required (part A)

1. **Registry (DM1).** `src/kernel/include/ciuki/display_driver.h` and `src/kernel/drivers/video/registry.c`. fbdev binds through the registry only; every `savage2d_*` weak symbol and call leaves `fbdev.c`.
   - The Savage registers through a new file, `src/kernel/drivers/video/s3savage_register.c`. That file fills `struct display_driver` with the existing exported Savage functions (init, fallback ops, eligibility, attempt marker, quarantine, record, snapshot, safe-mode summary, the sweep as self-test and the SuperSavage display provider).
   - **Do not edit** `savage2d.c`, `supersavage_display.c` or `supersavage_mode.c`: f2-99 is changing the cursor there. If a function you need is `static`, list it in the report instead of changing it. Part B moves the files after f2-99 merges.
2. **Command set and batches (DM2, DM3).**
   - Extend `accel_ops` with the optional MONO, INVERT, KEYED, SCALED, LINE and colour-cursor hooks, plus clip lists.
   - Add `CIUKI_ACCEL_BATCH` with fences to the ABI. Keep the existing requests working as wrappers.
   - Regenerate the API reference (`python3 scripts/gen_api_reference.py`).
3. **Software engine (DM4).** `src/kernel/drivers/video/swdib.c` implements every command with golden host tests at 16, 24 and 32 bpp. Punting per call means `-EOPNOTSUPP`, then a sync of the overlapping area, then software, with per-operation counters. The software engine is used when there is no driver, the command is unsupported, the engine is quarantined, a target is missing, in safe mode or with `accel=0`.
4. **Generic paths.**
   - **Video memory (DM5):** generic off-screen leases.
   - **Mode providers (DM6):** `display.c` calls the bound driver's mode provider and contains no `supersavage_*` call or Savage PCI id.
   - **Identity (DM7):** generic `ciuki_accel_info` and the device record.
   - **Attempt marker (DM8):** keyed by driver id and PCI id.
   - **Hardware check (DM9):** the generic accelerator test compares engine read-back with the software engine; the Savage sweep runs as that driver's self-test.
5. **User interface (DM10).**
   - The desktop notice and the Control Panel Display page read names from the device record and `/System/hwids/pci-ids.json`; remove the hand PCI tables in `apps/control/display.c` and `apps/control/app.c`.
   - `gfxdemo` prints the driver id.
   - Keep the edits to `apps/desktop/*.c` minimal, because f2-98a and f2-98b are changing those files.
6. **Enforcement and documents (DM12).** Add a host test for driver or chip names in generic code. Reword `README.md`, `docs/desktop.md`, `docs/build-and-run.md` (`accel=0` disables native 2D drivers) and `Roadmap.md`, then list the families in the contract table.
7. **Tests.**
   - Existing Savage, fbdev, display, hardware check, desktop accel notice and device manager host tests stay green, with goldens updated only where text changed by DM10.
   - New tests cover the registry, the software engine, punting, batches, the ABI and names.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f2-101-report.md`.

## Amendment 1 (lead, 2026-10-22 23:26): scope after the Codex plan review

The contract's Amendment 1 is binding and **narrows part A**.

**Part A, this run:**
- the registry with full lifecycle and lock rules;
- the generic safe-mode and `accel=0` policy;
- PCI ownership proof;
- adapter forwarding of the Savage ops table, with no static driver function referenced and the three Savage files untouched;
- the software reference engine for the **existing** commands only (FILL, COPY, PATTERN, WRITE; CPU paths moved out of fbdev with golden tests);
- the legacy ABI preserved plus a new negotiated generic query;
- device-record metadata and generic UI text;
- independent mode-provider binding;
- the generic hardware check proving native execution;
- explicit build listing;
- the name-enforcement test with explicit exceptions;
- the fake-driver integration tests of contract item 11.

Items 2 and 3 of "Required" shrink accordingly. **Not in part A:** new commands, clip lists, batches, fences, colour cursor, system-memory targets (part C, a separate directive after a semantic specification), keyed markers and the file move (part B, after f2-99 merges).
