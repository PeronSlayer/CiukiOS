# fx-flaky-clip: the DirectDraw production-seam test fails under the full Win32 suite

- **Written:** 2026-10-29 07:28 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `medium`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-flaky-clip`, branch `wt/fx-flaky-clip` from `main` at `2889623e2`.

## Observation

- In the lead's integration-9 rerun, `tests.host.win32.ddraw.test_ddraw.DirectDrawTests.test_native_adapter_on_production_user_seam` failed inside the full Win32 suite. The fixture aborted (-6) at its clipboard step:

  ```
  USER CHECK tests/host/win32/user32/g5b_clipboard_test.h:40: got_pal && got_meta && got_emf && got_pal!=pal && got_meta!=mfp && got_emf!=enh (error 120)
  ```

- Run alone, the same test on the same tree passes in 27 s. It also passed in the previous full-suite run.
- The full log is `build/lead-scratch/chain-integ9b/win32.log` in the main checkout.

## Scope

1. Find out why the palette, metafile and enhanced-metafile clipboard step can return error 120 (`ERROR_CALL_NOT_IMPLEMENTED`). Possible causes:
   - shared state with other tests: a clipboard store, temporary files, a fixed path or port;
   - a time bound under load;
   - an uninitialised or order-dependent path in the clipboard format synthesis.

   Reproduce it in the full suite's order, or under CPU load (`stress`-like busy loops at `nice -n 19`).
2. Fix the cause: production code if the synthesis can really fail, the test fixture if the state is shared. A rerun or a longer timeout is not a fix unless the bound itself is wrong, and then explain why.
3. Show the fix with 20 consecutive runs of the test under load and one full Win32 suite run.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-flaky-clip-report.md`.
