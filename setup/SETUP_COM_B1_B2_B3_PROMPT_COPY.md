# SETUP.COM Phase 4 Block B1/B2/B3 Prompt Copy

## Scope
Original English prompt copy for screens B1/B2/B3.
This copy supports success and failure paths plus concise cancel/confirm wording.

## B1 - Welcome Screen

### Primary text
- Title: `CiukiOS Setup`
- Body: `Welcome. This program installs CiukiOS to your selected drive.`
- Hints: `Enter: Next  Esc: Cancel`
- Optional hints (only when a menu cursor is present): `Up/Down: Move`

### Success-path messages
- `Welcome acknowledged. Opening component selection.`

### Failure-path messages
- `Setup resources are not ready. Cannot continue from welcome screen.`
- `Input error. Press a supported key to continue.`

## B2 - Component Selection (Minimal / Standard / Full)

### Primary text
- Title: `Select Components`
- Body: `Choose an installation profile:`
- Options:
  - `Minimal - core files only`
  - `Standard - recommended default set`
  - `Full - all available components`
- Hints: `Up/Down: Select profile  Enter: Next  Esc: Back`

### Success-path messages
- `Profile selected: {PROFILE}.`
- `Profile saved. Opening target drive selection.`

### Failure-path messages
- `No valid profile is selected.`
- `Profile data is unavailable. Retry or go back.`

## B3 - Target Drive Selection + Destructive Confirmation

### Primary text (target selection)
- Title: `Select Target Drive`
- Body: `Choose the drive for CiukiOS installation.`
- Hints: `Up/Down: Select drive  Enter: Continue  Esc: Back`

### Primary text (destructive confirmation)
- Title: `Confirm Drive Format`
- Warning: `Warning: Drive {DRIVE} will be formatted. All data on this drive will be lost.`
- Prompt: `Continue with this drive?`
- Confirm action: `Confirm and continue`
- Back action: `Back to drive list`
- Cancel action: `Cancel setup`

### Success-path messages
- `Drive selected: {DRIVE}.`
- `Destructive action confirmed. Continuing to installer workflow.`

### Failure-path messages
- `No eligible target drives were found.`
- `Drive scan failed. Check media and hardware, then retry.`
- `Selected drive is no longer available. Choose another drive.`

## Cancel / Abort / Confirmation Copy
- Cancel prompt: `Cancel setup now? No installation changes have been applied yet.`
- Abort confirmation: `Abort setup and return to DOS?`
- Retry prompt: `Operation failed. Retry, go back, or cancel setup.`
- Destructive confirmation (short): `Formatting {DRIVE} will erase all data. Continue?`
