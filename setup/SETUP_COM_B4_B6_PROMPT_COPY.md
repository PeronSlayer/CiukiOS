# SETUP.COM Phase 4 Block B4/B5 Prompt Copy

## Scope
Original English prompt copy for B4/B5 setup screens.
Includes progress, completion, failure, retry, and keyboard-hint text.

## B4 - Progress Screen

### Primary text
- Title: `Installing CiukiOS`
- Body: `Please wait while setup copies files to the selected target.`
- Context line: `Source media: {MEDIA_LABEL} ({MEDIA_ID})`
- Context line: `Current file: {CURRENT_FILE}`
- Context line: `Progress: {COPIED_FILES}/{TOTAL_FILES} files, {COPIED_BYTES}/{TOTAL_BYTES} bytes`
- Context line: `Last status: {STATUS_CODE}`

### Keyboard hints
- Pre-write state (`B4_PREWRITE_READY`) only: `Enter: Start copy  Esc: Back to B3 target selection`
- Running state (`B4_RUNNING`): `Esc: Cancel Setup`
- Completed state (`B4_COMPLETE`): `Enter: Next  Esc: Back to B4 summary`

### Progress and retry messages
- `Copying {CURRENT_FILE} from {MEDIA_LABEL}.`
- `Verifying copied data. Please wait.`
- `Copy paused at {CURRENT_FILE}.`
- `Read error on {CURRENT_FILE} from media {MEDIA_LABEL}.`
- `Check media and press Retry to continue.`
- `Retry failed. You can Retry again, go Back, or Cancel Setup.`
- Retry prompt: `Retry this step now?`
- Retry actions: `Retry`, `Back`, `Cancel Setup`

## B5 - Completion Screen

### Primary text
- Title: `Installation Complete`
- Body: `CiukiOS was installed successfully.`
- Summary line: `Profile: {PROFILE}`
- Summary line: `Target: {TARGET_PATH}`
- Summary line: `Files copied: {COPIED_FILES}`
- Summary line: `Bytes copied: {COPIED_BYTES}`
- Summary line: `Final status: OK`

### Keyboard hints
- `Enter: Next  Esc: Back`

### Completion messages
- `All selected components were installed.`
- `Setup summary is ready.`
- `Press Enter to continue to final setup handoff.`

## B5 - Failure Screen

### Primary text
- Title: `Installation Stopped`
- Body: `Setup cannot continue until this issue is resolved.`
- Detail line: `Step: {FAILED_STEP}`
- Detail line: `Status: {ERROR_CODE}`
- Detail line: `Last file/media: {LAST_CONTEXT}`

### Keyboard hints
- `Up/Down: Select action  Enter: Confirm  Esc: Back`

### Failure messages and actionable next steps
- `Action required: verify source media is present and readable.`
- `Action required: verify target drive is still available and writable.`
- `Action required: free space or select another target, then retry.`
- `Action required: if retries fail, use Cancel Setup and collect error code.`
- Retry prompt: `Retry failed step now?`
- Back prompt: `Return to previous safe screen?`
- Cancel prompt: `Cancel Setup and return to DOS?`
- Action labels: `Retry`, `Back`, `Cancel Setup`

## Shared short copy
- Generic retry: `Operation failed. Retry, go back, or Cancel Setup.`
- Generic cancel confirm: `Cancel Setup now?`
- Generic back confirm: `Return to the previous step?`