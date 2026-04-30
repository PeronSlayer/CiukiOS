# SETUP.COM Phase 4 Block B4/B5/B6 Screen Flow

## Scope
This document defines text-mode UX flow artifacts for block B4/B5/B6.
It extends B1/B2/B3 with progress, completion, failure, and keyboard-validation hooks.
No runtime core or workflow engine behavior is implemented here.

## Screen IDs
- `B4_PROGRESS`
- `B4_PROGRESS_RETRY_PROMPT`
- `B5_COMPLETE`
- `B5_FAILURE`
- `B6_KEYBOARD_VALIDATION_HOOKS` (non-visual instrumentation contract)

## Navigation Keys
- `Up`: move selection cursor up in lists or multi-action prompts.
- `Down`: move selection cursor down in lists or multi-action prompts.
- `Enter`: accept focused action.
- `Esc`: deterministic back/cancel per screen and state rules.

## Screen Behavior

### B4_PROGRESS
- Purpose: show deterministic install progress with current file/media context.
- Required fields:
  - Source media label and expected media id.
  - Current file path being copied.
  - Copied files/total files and copied bytes/total bytes.
  - Last operation status code.
- B4 progress state labels:
  - `B4_PREWRITE_READY`: target pre-write state; copy has not started and no target write is committed.
  - `B4_RUNNING`: copy has started and target writes may already exist.
  - `B4_COMPLETE`: copy+verify reached deterministic completion.
- Primary actions:
  - `Enter` on `Start copy` in `B4_PREWRITE_READY` -> `B4_PROGRESS` (`B4_RUNNING`).
  - `Enter` on `Next` (enabled only in `B4_COMPLETE`) -> `B5_COMPLETE`.
  - `Esc` in `B4_PREWRITE_READY` -> `Back` to `B3_TARGET_SELECT`.
  - `Esc` in `B4_RUNNING` -> open `Cancel Setup` prompt; on confirm, abort setup and return to DOS.
  - `Esc` in `B4_COMPLETE` -> `Back` to `B4_PROGRESS` read-only summary (`B4_COMPLETE`).
- Failure transition:
  - Media read/copy error opens `B4_PROGRESS_RETRY_PROMPT` with failed file context.

### B4_PROGRESS_RETRY_PROMPT
- Purpose: deterministic handling for transient media/copy failures.
- Primary actions:
  - `Enter` on `Retry` -> retry same file copy in `B4_PROGRESS`.
  - `Enter` on `Back` or `Esc` -> previous safe step:
    - `B3_TARGET_SELECT` if no target write was committed.
    - `B4_PROGRESS` paused view if writes already started.
  - `Enter` on `Cancel Setup` -> abort setup and return to DOS.

### B5_COMPLETE
- Purpose: show successful installation summary and deterministic next action.
- Required summary fields:
  - Selected profile.
  - Target drive/path.
  - Copied file count and byte totals.
  - Final status code (`OK`).
- Primary actions:
  - `Enter` on `Next` -> workflow completion handoff (stream C finalize step).
  - `Esc` or `Enter` on `Back` -> return to `B4_PROGRESS` read-only summary.
  - `Cancel Setup` and `Retry` are not available on completion.

### B5_FAILURE
- Purpose: show terminal or recoverable failure with actionable next steps.
- Required fields:
  - Failure status code.
  - Failed step id.
  - Last processed file/media context (if available).
- Primary actions:
  - `Enter` on `Retry` -> retry failed step when failure class is recoverable.
  - `Enter` on `Back` or `Esc` -> previous safe screen (`B4_PROGRESS` or `B3_TARGET_SELECT`).
  - `Enter` on `Cancel Setup` -> abort setup and return to DOS.
  - `Next` is disabled until failure is cleared by retry success.

### B6_KEYBOARD_VALIDATION_HOOKS
- Purpose: define keyboard validation insertion points for B1-B5 without runtime changes.
- Hook points:
  - `HOOK_KEY_CAPTURE(screen_id, key_code)`
  - `HOOK_ACTION_RESOLVE(screen_id, action_id)`
  - `HOOK_TRANSITION(from_screen, action_id, to_screen)`
  - `HOOK_RETRY_ATTEMPT(step_id, retry_count, status_code)`
- Hook contract:
  - Every `Enter`, `Esc`, `Up`, and `Down` event in B1-B5 must emit key capture + action resolve records.
  - Every retry path must emit retry attempt record with deterministic status code.

## Transition Matrix (B4/B5)
| From | Action | Transition label | To |
|---|---|---|---|
| `B4_PROGRESS` (`B4_PREWRITE_READY`) | `Enter` on Start copy | Start copy | `B4_PROGRESS` (`B4_RUNNING`) |
| `B4_PROGRESS` (`B4_COMPLETE`) | `Enter` on Next | Next | `B5_COMPLETE` |
| `B4_PROGRESS` (`B4_PREWRITE_READY`) | `Esc` | Back | `B3_TARGET_SELECT` |
| `B4_PROGRESS` (`B4_RUNNING`) | `Esc` + confirm on `Cancel Setup` prompt | Cancel Setup | Exit to DOS |
| `B4_PROGRESS` (`B4_COMPLETE`) | `Esc` or Back | Back | `B4_PROGRESS` (`B4_COMPLETE`, read-only summary) |
| `B4_PROGRESS` | copy/media error | Retry path available | `B4_PROGRESS_RETRY_PROMPT` |
| `B4_PROGRESS_RETRY_PROMPT` | `Enter` on Retry | Retry | `B4_PROGRESS` |
| `B4_PROGRESS_RETRY_PROMPT` | `Esc` or Back | Back | `B4_PROGRESS` or `B3_TARGET_SELECT` |
| `B4_PROGRESS_RETRY_PROMPT` | `Enter` on Cancel Setup | Cancel Setup | Exit to DOS |
| `B5_COMPLETE` | `Enter` on Next | Next | Stream C finalize/exit step |
| `B5_COMPLETE` | `Esc` or Back | Back | `B4_PROGRESS` (read-only summary) |
| `B5_FAILURE` | `Enter` on Retry (recoverable) | Retry | `B4_PROGRESS` |
| `B5_FAILURE` | `Esc` or Back | Back | `B4_PROGRESS` or `B3_TARGET_SELECT` |
| `B5_FAILURE` | `Enter` on Cancel Setup | Cancel Setup | Exit to DOS |

## Dependencies
- Stream C dependencies:
  - Owns state machine transitions, retry classification, and finalization handoff.
  - Owns deterministic status/failure code emission used by `B4_PROGRESS` and `B5_FAILURE`.
- Stream D dependencies:
  - Owns target/media validity checks that gate retry/back routing when destination state changes.
  - Owns filesystem preflight/post-write checks used in failure action recommendations.

## Out-of-Scope (Handled by Stream C/D Implementation)
- Runtime implementation of key hooks and transition guards.
- Actual file copy execution, media-swap orchestration, and rollback behavior.
- Persistent install report generation.