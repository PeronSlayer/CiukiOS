# SETUP.COM Phase 4 Block B1/B2/B3 Screen Flow

## Scope
This document opens operational block B1/B2/B3 of the setup checklist.
It defines only text-mode UX flow artifacts for the first three screens.
No runtime core or workflow engine behavior is implemented here.

## Screen IDs
- `B1_WELCOME`
- `B2_COMPONENT_SELECT`
- `B3_TARGET_SELECT`
- `B3_CONFIRM_DESTRUCTIVE`
- `B3_TARGET_ERROR` (failure handling surface for target scan/validation)

## Navigation Keys
- `Up`: move selection cursor up.
- `Down`: move selection cursor down.
- `Enter`: accept current selection, advance, or confirm action.
- `Esc`: back or cancel depending on screen context.

## Screen Behavior

### B1_WELCOME
- Purpose: entry point with concise setup purpose and key hints.
- Primary actions:
  - `Enter` -> Next to `B2_COMPONENT_SELECT`.
  - `Esc` -> Cancel prompt, then abort to DOS if confirmed.
- Key hints shown on screen: `Up/Down` (if menu cursor present), `Enter = Next`, `Esc = Cancel`.

### B2_COMPONENT_SELECT
- Purpose: choose install profile: `Minimal`, `Standard`, or `Full`.
- Primary actions:
  - `Up/Down` -> change profile selection.
  - `Enter` -> Next to `B3_TARGET_SELECT` with selected profile persisted in setup session state.
  - `Esc` -> Back to `B1_WELCOME`.
- Transition labels:
  - Next: profile accepted.
  - Back: return without committing target settings.

### B3_TARGET_SELECT
- Purpose: select install target drive and gate entry to destructive step.
- Primary actions:
  - `Up/Down` -> move across detected eligible targets.
  - `Enter` -> open `B3_CONFIRM_DESTRUCTIVE` for selected drive.
  - `Esc` -> Back to `B2_COMPONENT_SELECT`.
- Failure surface:
  - if target scan fails or no eligible targets are available, show `B3_TARGET_ERROR`.

### B3_CONFIRM_DESTRUCTIVE
- Purpose: explicit destructive confirmation before continuing toward format/copy path.
- Primary actions:
  - `Enter` on `Confirm` -> Next to downstream workflow step (outside this block).
  - `Esc` or `Enter` on `Back` -> Back to `B3_TARGET_SELECT`.
  - `Enter` on `Cancel Setup` -> Cancel prompt -> abort if confirmed.
- Required warning: selected drive will be formatted and existing data will be lost.

### B3_TARGET_ERROR
- Purpose: handle drive detection/validation failure in B3.
- Primary actions:
  - `Enter` on `Retry` -> retry detection and return to `B3_TARGET_SELECT` if successful.
  - `Enter` on `Back` or `Esc` -> `B2_COMPONENT_SELECT`.
  - `Enter` on `Cancel` -> setup abort prompt.
- Transition labels:
  - Retry: re-run target discovery.
  - Back: preserve selected component profile and return to B2.
  - Cancel: terminate setup session.

## Transition Matrix (B1/B2/B3)
| From | Action | Transition label | To |
|---|---|---|---|
| `B1_WELCOME` | `Enter` | Next | `B2_COMPONENT_SELECT` |
| `B1_WELCOME` | `Esc` + confirm | Cancel | Exit to DOS |
| `B2_COMPONENT_SELECT` | `Enter` | Next | `B3_TARGET_SELECT` |
| `B2_COMPONENT_SELECT` | `Esc` | Back | `B1_WELCOME` |
| `B3_TARGET_SELECT` | `Enter` on drive | Next | `B3_CONFIRM_DESTRUCTIVE` |
| `B3_TARGET_SELECT` | `Esc` | Back | `B2_COMPONENT_SELECT` |
| `B3_TARGET_SELECT` | scan failure | Retry path available | `B3_TARGET_ERROR` |
| `B3_TARGET_ERROR` | `Enter` on Retry | Retry | `B3_TARGET_SELECT` |
| `B3_TARGET_ERROR` | `Esc` or Back | Back | `B2_COMPONENT_SELECT` |
| `B3_TARGET_ERROR` | `Enter` on Cancel | Cancel | Exit to DOS |
| `B3_CONFIRM_DESTRUCTIVE` | `Enter` on Confirm | Next | Stream C entry point |
| `B3_CONFIRM_DESTRUCTIVE` | `Esc` or Back | Back | `B3_TARGET_SELECT` |
| `B3_CONFIRM_DESTRUCTIVE` | `Enter` on Cancel Setup | Cancel | Exit to DOS |

## Dependencies
- Section A contracts should be frozen before wiring executable behavior:
  - installer input manifest fields
  - installer output report fields
  - critical-path dependency ownership
- B3 target listing depends on disk eligibility rules defined by Phase 4 section D.
- Transition handoff from `B3_CONFIRM_DESTRUCTIVE` to execution requires workflow state ownership from stream C.

## Known Out-of-Scope Items (Stream C)
- Deterministic workflow state machine implementation.
- Guard checks before every step transition.
- Safe rollback points for retry/back/cancel.
- Structured status/failure code emission.
- Timeout-safe prompt handling and media swap orchestration.
