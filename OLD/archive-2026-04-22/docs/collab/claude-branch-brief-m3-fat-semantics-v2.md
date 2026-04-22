# Claude Branch Brief - M3 FAT Semantics v2

Branch:
- `feature/claude-m3-fat-semantics-v2`

Mission:
- Advance roadmap M3 by hardening FAT write/path semantics with deterministic DOS-like outcomes.

You own:
- `stage2/src/fat.c`
- `stage2/include/fat.h`
- `scripts/test_fat_compat.sh`
- `docs/handoffs/2026-04-16-m3-fat-semantics-v2.md` (create/update)

Do now:
1. Finalize write-path safety:
   - invalid cluster chains
   - out-of-space handling
   - partial write safety/no metadata corruption
2. Finalize path/error semantics:
   - root / `.` / `..`
   - invalid 8.3 names
   - directory vs file misuse
   - deterministic status buckets: not found / exists / invalid / denied / not empty
3. Strengthen compatibility checks in `test_fat_compat.sh` with explicit assertions.

Constraints:
1. Do not revert changes from `main`.
2. Keep shell/UI refactors out of scope in this branch.
3. Preserve fallback behavior if FAT is unavailable.

Done when:
1. `make test-stage2` PASS
2. `make test-fallback` PASS
3. `make test-fat-compat` PASS
4. Handoff markdown includes:
   - changed files
   - edge cases covered
   - known remaining gaps
