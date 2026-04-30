# SETUP.COM Phase 4 Block B6 Keyboard Validation

## Validation Mode
- Baseline type: `document baseline`
- Runtime execution: `not executed in this document`
- Scope: keyboard navigation expectations for screens B1-B5.

## Expected Key Semantics
- `Up/Down`: move focus between selectable options where lists/actions exist.
- `Enter`: confirm focused action and execute deterministic transition.
- `Esc`: deterministic back/cancel according to current screen/state rules.
- `Retry path`: available in error/recovery prompts only.

## Expectation Matrix (Document Baseline)
No case below is marked runtime PASS/FAIL in this document. `Baseline status` means documented expectation only.

| Case ID | Screen | Key(s) | Expected behavior | Baseline status | Runtime evidence placeholder |
|---|---|---|---|---|---|
| `KB-B1-01` | `B1_WELCOME` | `Enter` | Transition Next to `B2_COMPONENT_SELECT`. | Defined (runtime pending) | `[ ] Attach transition log` |
| `KB-B1-02` | `B1_WELCOME` | `Esc` | Open cancel confirmation; abort on confirm. | Defined (runtime pending) | `[ ] Attach cancel prompt screenshot` |
| `KB-B2-01` | `B2_COMPONENT_SELECT` | `Up/Down` | Move selection across `Minimal/Standard/Full`. | Defined (runtime pending) | `[ ] Attach selection movement log` |
| `KB-B2-02` | `B2_COMPONENT_SELECT` | `Enter` | Persist profile and transition Next to `B3_TARGET_SELECT`. | Defined (runtime pending) | `[ ] Attach profile persistence log` |
| `KB-B2-03` | `B2_COMPONENT_SELECT` | `Esc` | Transition Back to `B1_WELCOME`. | Defined (runtime pending) | `[ ] Attach back transition log` |
| `KB-B3-01` | `B3_TARGET_SELECT` | `Up/Down` | Move selection across eligible target drives. | Defined (runtime pending) | `[ ] Attach drive cursor log` |
| `KB-B3-02` | `B3_TARGET_SELECT` | `Enter` | Open destructive confirmation for selected drive. | Defined (runtime pending) | `[ ] Attach confirmation screen evidence` |
| `KB-B3-03` | `B3_TARGET_SELECT` | `Esc` | Transition Back to `B2_COMPONENT_SELECT`. | Defined (runtime pending) | `[ ] Attach back transition log` |
| `KB-B3-04` | `B3_TARGET_ERROR` | `Enter` on Retry | Retry target detection and return to `B3_TARGET_SELECT` on success. | Defined (runtime pending) | `[ ] Attach retry attempt log` |
| `KB-B4-01` | `B4_PROGRESS` (`B4_PREWRITE_READY`) | `Enter` on Start copy | Transition to `B4_PROGRESS` (`B4_RUNNING`). | Defined (runtime pending) | `[ ] Attach start-copy transition log` |
| `KB-B4-02` | `B4_PROGRESS` (`B4_PREWRITE_READY`) | `Esc` | Transition Back to `B3_TARGET_SELECT`. | Defined (runtime pending) | `[ ] Attach prewrite back transition log` |
| `KB-B4-03` | `B4_PROGRESS` (`B4_RUNNING`) | `Esc` | Open `Cancel Setup` prompt; abort setup on confirm. | Defined (runtime pending) | `[ ] Attach running cancel flow log` |
| `KB-B4-04` | `B4_PROGRESS` (`B4_COMPLETE`) | `Enter` on Next | Transition Next to `B5_COMPLETE`. | Defined (runtime pending) | `[ ] Attach complete-next transition log` |
| `KB-B4-05` | `B4_PROGRESS` (`B4_COMPLETE`) | `Esc` | Transition Back to `B4_PROGRESS` read-only summary (`B4_COMPLETE`). | Defined (runtime pending) | `[ ] Attach complete back transition log` |
| `KB-B4-06` | `B4_PROGRESS_RETRY_PROMPT` | `Up/Down` | Move action focus across `Retry/Back/Cancel Setup`. | Defined (runtime pending) | `[ ] Attach action focus log` |
| `KB-B4-07` | `B4_PROGRESS_RETRY_PROMPT` | `Enter` on Retry | Retry current failed copy step and return to `B4_PROGRESS`. | Defined (runtime pending) | `[ ] Attach retry transition log` |
| `KB-B4-08` | `B4_PROGRESS_RETRY_PROMPT` | `Esc` or Back | Return to previous safe screen per rollback rule. | Defined (runtime pending) | `[ ] Attach rollback decision log` |
| `KB-B5-01` | `B5_COMPLETE` | `Enter` | Transition Next to stream C handoff step. | Defined (runtime pending) | `[ ] Attach handoff transition log` |
| `KB-B5-02` | `B5_COMPLETE` | `Esc` | Transition Back to `B4_PROGRESS` read-only summary. | Defined (runtime pending) | `[ ] Attach completion back transition` |
| `KB-B5-03` | `B5_FAILURE` | `Up/Down` | Move action focus across `Retry/Back/Cancel Setup`. | Defined (runtime pending) | `[ ] Attach failure action focus log` |
| `KB-B5-04` | `B5_FAILURE` | `Enter` on Retry | Retry failed step when class is recoverable. | Defined (runtime pending) | `[ ] Attach recoverable retry log` |
| `KB-B5-05` | `B5_FAILURE` | `Esc` or Back | Return to `B4_PROGRESS` or `B3_TARGET_SELECT` per safe state. | Defined (runtime pending) | `[ ] Attach safe-state back transition` |

## Pending Runtime Verification Items
- Verify key capture hooks emit one event per keypress for B1-B5.
- Verify action resolution hooks map keypress to deterministic action ids.
- Verify transition hooks report correct `from/action/to` tuples for all documented cases.
- Verify retry hook increments retry count and persists status code across attempts.
- Capture evidence artifacts (screenshots/log extracts) for all placeholders in this matrix.
- Re-run matrix after stream C state machine and stream D target/media checks are integrated.