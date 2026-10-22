# f6-04 — NDIS host step 1: kernel PE driver loader sharing the F5 parser, export allowlist ABI, callback containment

- **Phase/model:** F6, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f6-04` from `main`.
- **Owner decision (2026-10-22 ~23:10):** after the drawing commands, the NDIS host runs the network drivers of the owner's pack on any PC. The plan was agreed on 2026-10-13 (f6-03).
- **Contract:** `docs/design/ndis-miniport-host-contract.md`, binding. Read it completely; this directive is the f6-04 row of ND13 and rule ND3.

## Required

1. **One shared PE mechanism (ND3).**
   - Factor the bounded PE32 parser of `sdk/win32/ldr/pe.c` into a mechanism library with policy and allocator callbacks, compiled into both the F5 user loader and the kernel. That file is LGPL-2.1-or-later, which the GPL-2.0-only kernel may use: state this in the file header.
   - Shared parts: byte readers, overflow and extent checks, section layout, data directories, name and ordinal thunks, original-first-thunk fallback, and relocation validation and application.
   - Keep the F5 behaviour and fixtures unchanged. The tests of `tests/host/win32/` that cover the loader stay green.
   - The kernel `src/kernel/proc/win32/pe.c` either uses the library or is documented as a separate recognition step.
2. **Kernel driver loader policy.**
   - Accepts native subsystem 1, i386 PE32 only, with `DriverEntry` inside an executable section.
   - Uses supervisor mappings, pinned image memory, and checked relocations; an image without relocations needs its preferred base.
   - Rejects TLS. Bound imports are re-resolved. Delay imports are resolved eagerly or refused with a reason.
   - Rolls back completely before entry on any failure.
   - Lives in `src/kernel/ndis/` (new): `ntimage.c` plus a header.
3. **Export allowlist ABI.**
   - A table of native shim exports by module (`NDIS.SYS`, `NTOSKRNL.EXE`, `HAL.DLL`) with name, ordinal, calling convention (stdcall with stack bytes, fastcall `Kf*`/`Kef*`, cdecl) and kind (function or data, for example `KeTickCount`).
   - The whole import set is resolved before entry. There are no catch-all stubs, and an unknown name or ordinal refuses the load with a record `[ndis] load refused driver=<file> import=<module>!<name> reason=…`.
   - `MmGetSystemRoutineAddress` and `NdisGetRoutineAddress` use the same table.
   - Ship the table with only the exports f6-05 and f6-06 will implement, marked unimplemented; the loader refuses any image that imports one until they exist.
4. **Callback containment and fence prerequisite (ND13 f6-04 row).** Calls into driver code go through one thunk layer that records entry and exit, enforces the IRQL model of ND6 as a state machine, and turns a fault inside driver code into quarantine of that driver with its bus fenced. Use the existing fault-containment and registry quarantine paths, with no new `panic()`. A nonreturning callback is detected by the existing watchdog model.
5. **Host tests on synthetic fixtures only, never the owner's pack.**
   - Hostile layouts: overlapping sections, out-of-range directories, relocation overflow, malformed thunks.
   - Policy separation: a user image refused by the kernel policy and a driver image refused by the user policy.
   - Import resolution, ordinals, data exports and stdcall/fastcall thunk ABI on i386.
   - Entry rollback and fault and nonreturn containment in the model.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (Microsoft PE/COFF specification, NDIS 5.1 DDK pages). Host tests one at a time with `nice -n 19`. Write `build/codex/f6-04-report.md`.
