# sec-integ-fix: kernel suite failures on the security integration branch

- **Written:** 2026-10-27 13:57 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Branch `wt/sec-integ` (main 6a9abd114 + sec-int01 + sec-k02).
- The strict kernel suite (`scripts/test/host_kernel_tests.sh`) fails in `tests/host/test_app_gate.py`: `test_gate_launch_environment_matches_metadata`, `test_named_identity_growth_is_exactly_attributed`, `test_observed_physical_growth_is_attributed_to_measured_heap_pool` (lead log: /home/peronslayer/Desktop/CiukiOS/build/lead-scratch/integ-kernelsuite.log around line 3694). Main passes them; the security merges (per-process credential, fd tables on pages from fx-r3dalloc, account service) changed what the gate measures.
- Find whether the security code or the test model is wrong. Fix production code if a real attribution or environment defect exists (the user-security contract docs/design/user-security-contract.md governs what a launch environment must carry); otherwise update the gate model with the reason. Then run the whole kernel suite once and report every remaining failure.
Report to build/codex/sec-integ-fix-report.md. Leave changes uncommitted.
