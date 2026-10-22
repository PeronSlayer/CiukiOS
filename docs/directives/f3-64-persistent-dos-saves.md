# f3-64 — Persistent DOS game saves (per-title persistent overlays)

- **Phase/model:** F3, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f3-64`, created after f3-63 merges.
- **Source:** decision 1 of the f3-63 report. The DOS game disk is a copy-on-write overlay that is discarded when the session ends, even after an orderly close, so **game saves, high scores and configuration (SETUP.EXE choices) are lost**. Lead decision (2026-10-23 00:17): saves must persist across sessions, as on a real PC and in a Windows 98 DOS box.
- **Contract:** `docs/design/unified-dos-contract.md` (COW namespaces, kernel/base/content identities, f3-47 provider/cache/overlay) and `docs/design/win98-class-resource-management.md` (write-behind, flush, clean dismount). Read both completely.

## Required

1. **Persistent overlay per title.** Each game's overlay is stored on the system volume under the DOS game store, keyed by kernel, base and content identity as the contract requires.
   - It is kept across sessions and reboots and reattached when the same title starts again.
   - An overlay whose identity no longer matches (changed base image, kernel or filesystem identity) is never reused silently: it is preserved under a dated name, and the game starts fresh with a record.
2. **Crash consistency.** The overlay commits with an ordered write and flush, so a power loss leaves either the last committed state or the previous one, never a mixed state.
   - An orderly close flushes and seals.
   - A forced end (crash, kill) keeps the last committed state and records `forced-session-end` with what was lost.
   - Host tests include crash cuts at every write.
3. **User controls.** In the game's properties (the desktop's existing per-game panel, without bringing back the removed DOS Box settings UI):
   - **Reset game data** discards the overlay after a confirmation;
   - **Export saves** copies the changed files out to a folder;
   - the overlay size is shown.
4. **Quota.** A per-title size limit (default 64 MiB, measured against the T23 system volume) with a clear message when full; the session continues read-only for new writes rather than corrupting.
5. **Tests.**
   - Host tests: save, close, restart and see the saved file; identity mismatch preservation; crash cuts; quota; reset; export; two titles isolated from each other.
   - The existing DOS conformance and unified-DOS tests stay green.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f3-64-report.md`.
