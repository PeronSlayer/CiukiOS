# OpenGEM absolute-path preload probe (OPENGEM-013)

## Context
OPENGEM-012 added lexical classification of the resolved path with a
capability verdict, but no file bytes were ever loaded from the
absolute path. OPENGEM-013 closes that gap: CiukiOS now actually
`fat_read_file()`s the resolved binary into the runtime payload
buffer at `SHELL_RUNTIME_COM_ENTRY_ADDR`, inspects the first bytes
to establish the real on-disk signature, and publishes a dispatch
verdict that a real native loader (OPENGEM-014+) will consume.

Execution still defers to `shell_run()` — this is the observability
baseline for *real I/O*, not the dispatcher.

## Change summary

1. **New helper** `stage2_opengem_preload_absolute(path, expect_size, classify)`
   in `stage2/src/shell.c`:
   - Guards: `no-path`, `preload-empty`, `preload-too-large`,
     `preload-io-error` — emitted as `status=<token>` on the read
     marker and mirrored in the verdict reason.
   - Calls `fat_read_file(path, SHELL_RUNTIME_COM_ENTRY_ADDR, SHELL_RUNTIME_COM_MAX_PAYLOAD, &read_bytes)`.
   - Inspects first 2 bytes of the staged buffer:
     - `MZ` → signature=`MZ`
     - `ZM` → signature=`ZM` (MZ variant)
     - printable ASCII → signature=`text`
     - zero bytes read → signature=`empty`
     - otherwise → signature=`unknown`
   - Cross-checks signature vs. OPENGEM-012 classify label and
     picks a verdict+reason.

2. **Frozen marker set** (append-only):
   ```
   OpenGEM: preload begin path=<p> expect_size=0x<hex32>
   OpenGEM: preload read bytes=0x<hex32> status=<ok|too-large|io-error|no-path>
   OpenGEM: preload signature=<MZ|ZM|text|empty|unknown> match=<0|1>
   OpenGEM: preload verdict=<dispatch-native|defer-to-shell-run> reason=<token>
   OpenGEM: preload complete
   ```

3. **Stable verdict-reason tokens** (disjoint from OPENGEM-012):
   | Token | Meaning |
   |-------|---------|
   | `preload-empty` | zero-byte file |
   | `preload-too-large` | exceeds payload window |
   | `preload-io-error` | `fat_read_file` failed |
   | `preload-no-path` | no resolved path from preflight |
   | `signature-mismatch` | classify expected MZ but signature is not |
   | `mz-16bit-pending` | MZ confirmed, extender not yet live |
   | `bat-interp-ready` | BAT confirmed, interp will run it |
   | `com-runtime-ready` | COM confirmed, runtime will run it |
   | `unsupported-app` | `.APP` — no loader |
   | `unsupported-unknown` | fell through the ladder |

4. **Invocation**: `shell_run_opengem_interactive()` calls the
   preload helper immediately after the OPENGEM-012 classify probe
   and before `shell_run()`. Ordering pipeline:
   `dispatch target` → `extender probe` → `classify` → `preload` → `shell_run`.

## Files touched
- `stage2/src/shell.c` — preload helper + invocation + a small
  lexical-classify re-derivation at the call site to pass the
  label by value (avoids exposing classify's internal label as a
  module-global).
- `scripts/test_opengem_preload.sh` — new gate (37 OK / 0 FAIL).
- `Makefile` — target `test-opengem-preload`.

## Gate assertions (static)
- `OPENGEM-013` sentinel.
- Preload function + `fat_read_file(` + `SHELL_RUNTIME_COM_ENTRY_ADDR`.
- All five marker prefixes with their tokens
  (`expect_size=0x`, `status=`, `match=`, `reason=`).
- All four status labels, all five signature labels, both verdict
  literals, all 10 reason tokens.
- Invocation ordering `classify → preload → shell_run`.
- First-occurrence marker order
  `begin < read < signature < verdict < complete`.
- Makefile target declared.

## Runtime (opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`)
- `begin … expect_size=0x<8-digit-hex>` well-formed.
- `read bytes=0x<8-digit-hex> status=<ok|too-large|io-error|no-path>`.
- `signature=<MZ|ZM|text|empty|unknown> match=<0|1>`.
- `verdict=<dispatch-native|defer-to-shell-run> reason=<kebab>`.

## Risks
- The preload writes into the runtime payload buffer, so when
  `shell_run()` later calls `shell_run_from_fat()`, the buffer is
  re-populated from scratch. Not a correctness issue (the buffer is
  shell-owned), but it does double the FAT I/O for this path. An
  optimization (reuse the already-staged bytes) is deferred to
  OPENGEM-014 where we take ownership of dispatch.
- Signature classification is still coarse (2-byte peek). Good
  enough to distinguish MZ from non-MZ; insufficient for PE/ELF
  discrimination — out of scope today.
- Adding `dispatch-native` as a literal makes it a public contract
  even though no branch emits it yet. This is intentional so the
  gate already validates the future vocabulary.

## Next step
- OPENGEM-014: real native absolute-path dispatcher. When the
  preload verdict is `defer-to-shell-run reason=(bat-interp-ready|com-runtime-ready)`,
  skip `shell_run()` entirely and hand control to
  `shell_run_batch_file()` (for BAT) or `shell_run_staged_image()`
  (for COM) on the already-staged buffer, flipping the verdict
  emitter to `dispatch-native`.
