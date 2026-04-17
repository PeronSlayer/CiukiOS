# SR-EDIT-001 - CIUKEDIT.COM (Line-Oriented Editor)

## Goal
Provide a native CiukiOS editor binary (`CIUKEDIT.COM`) that can create, open, edit, and save plain-text files from the shell using the already available INT 21h surface.

## ABI Surface Used (INT 21h)
- `AH=09h`: banner/help output (`$`-terminated strings)
- `AH=0Ah`: buffered line input for editor prompt
- `AH=3Dh`: open existing file for read
- `AH=3Fh`: read file content
- `AH=3Ch`: create/truncate output file
- `AH=40h`: write to stdout and file handle
- `AH=3Eh`: close file handle

No new INT 21h functions are added in stage2.

## UX Command Reference
- Plain text input: append line to buffer
- `:w`: save buffer to file
- `:q`: quit without saving
- `:wq`: save and quit
- `:l`: list buffer with 1-based line numbers
- `:d N`: delete line `N` (1-based)
- `:h`: print help line

Runtime markers emitted by the editor:
- `[edit] open path=... lines=N bytes=M`
- `[edit] open path=... new=1`
- `[edit] save path=... lines=N bytes=M`
- `[edit] quit dirty=0|1`
- `[edit] warn class=no_filename default=UNTITLED.TXT`
- `[edit] error class=...`

## File Rules and Determinism
- Read path accepts both `\n` and `\r\n` line endings.
- Write path always normalizes output to `\n` line endings.
- Save operation rewrites file from current in-memory buffer (`create/truncate` semantics).

## Known Limits
- Max lines: `200`
- Max chars per line: `128`
- Single file in memory only
- Line-oriented interface (no full-screen cursor editing)

## Failure Taxonomy
- `bad_command`: unknown `:` command
- `bad_index`: invalid `:d N` index
- `buffer_full`: line-capacity overflow
- `open/read/write`: file I/O failures with deterministic `rc=0xXX`
- `parse`: malformed startup/input parse issues

Exit code mapping:
- `0x00`: clean exit (`:q`, `:wq` success)
- `0x01`: save/open/read/write failure
- `0x02`: parse-level failure
