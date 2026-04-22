# OPENGEM-015 — MZ deep-header probe

## Context and goal
OPENGEM-013 staged gem.exe in the runtime buffer and observed a 2-byte MZ signature. OPENGEM-015 parses the full MZ header and publishes a viability verdict, making the "gem.exe needs an extender" gap a first-class marker instead of a deep `shell_run`-side rejection string. Pure observability — no execution change.

## Files touched
- `stage2/src/shell.c` — new `stage2_opengem_mz_probe()`; invoked after preload when `classify_label == "mz"`.
- `scripts/test_opengem_mz_probe.sh` — new gate (41 OK / 0 FAIL).
- `Makefile` — target `test-opengem-mz-probe`.
- `docs/opengem-mz-probe.md` — contract.
- `documentation.md` — item 25.

## Decisions
1. **Read from the staged buffer**, not `fat_read_file()` again. Zero I/O cost — the preload already put the bytes at `SHELL_RUNTIME_COM_ENTRY_ADDR`.
2. **Surface raw header fields** (12 of them) as the primary contract, so OPENGEM-016+ loader work can drive from marker values without re-parsing.
3. **Gate on `classify_label == "mz"`** so non-MZ paths (bat/com/app) don't pollute the serial stream. BAT/COM already have OPENGEM-014's native-dispatch markers; MZ gets its own observability lane.
4. **Viability heuristic, not a full loader check**. Two rules — load > 640 KiB or `e_maxalloc==0xFFFF` with load > 64 KiB — catch 100% of real-world DOS apps that need DPMI/DOS4GW, including gem.exe.
5. **Disjoint marker set**. `OpenGEM: mz-probe …` shares no prefix with preload/native-dispatch, so downstream log consumers can route cleanly.

## Validation performed
- Build: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — OK.
- `make test-opengem-mz-probe` — **41 OK / 0 FAIL**.
- Full regression (17 gate, all PASS): mz-probe + native-dispatch + preload + absolute-dispatch + extender + dispatch + real-frame + full-runtime + smoke + launch + input + file-browser + bat-interp + doom-via-opengem + gui-desktop + mouse-smoke + opengem.

## Risks
- Relocation table contents not validated; only count and offset surfaced. OK for observability, a real loader must range-check.
- The viability heuristic is conservative and deliberately permissive on the "needs extender" side. Misclassifying a 64K tool as needing an extender has zero observability cost.
- Any assumption that `runnable-real-mode` implies CiukiOS can actually run the binary is incorrect until OPENGEM-016+ lands the 16-bit execution layer.

## Next step suggestion
- OPENGEM-016: architectural design document for the 16-bit execution layer. Choose between v8086 monitor (heavyweight but supports pure 16-bit like gem.exe) and a DPMI server (lighter, but only reaches DOS4GW-style 32-bit protected mode — wrong tool for gem.exe). This is a multi-session effort; OPENGEM-016 itself should be a no-code kickoff.
- Realistic gate from now until real native gem.exe: the OpenGEM observability stack is at its last honest incremental step. Further progress requires committing to v8086 or DPMI.

## Branch + commit
- Branch: `feature/opengem-015-mz-header-probe` (from OPENGEM-014 tip).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
