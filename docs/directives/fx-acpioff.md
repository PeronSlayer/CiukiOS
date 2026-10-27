# fx-acpioff: ACPI S5 soft-off fallback

Original task: see build/codex/fx-acpioff-report.md (implemented by a Claude agent, committed 14d05bec8).

## Lead amendment 1 (2026-10-26 23:32): review round 1

Fix every finding of the Codex review in build/codex/fx-acpioff-review-latest.md, each with a regression test:
- P1: firmware ownership must be restored when S5 fails. Record the incoming mode and enable masks, require a rollback command before taking ownership, roll back within a bound before returning, and keep an explicit "ownership uncertain" state if the rollback fails.
- P2: read both PM1 control blocks (A OR B) for mode checks and preserved bits, and write each block's SLP_TYP separately.
- Fix the remaining items exactly as the review describes them, including telling unsupported registers apart from absent ones.

Run the acpi and power host suites and the desktop shutdown test with nice -n 19. Report as "Review round 1". Leave changes uncommitted.
