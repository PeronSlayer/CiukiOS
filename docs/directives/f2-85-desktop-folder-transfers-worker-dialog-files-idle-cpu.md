# f2-85 — the desktop folder still copies on the shell's UI thread without any dialog; Files burns half the CPU while another process copies

- **Phase:** F2 (desktop shell, applications). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-85` from main (after `3357582f`).
- **Authority:** `apps/desktop/folder.c` (desktop folder transfers), `apps/desktop/` only for the dialog hosting and the close-deadline deferral of item 3, `apps/files/` (`app.c` media poll, `transfer.c`), `apps/libdeskui` only if the shared transfer dialog needs a shell-side host, `tests/host/desktop/`, `docs/design/desktop-shell.md`.
- **Not yours:** the kernel, the storage layer (f1-82), Music (f4-34).

## Evidence (T23, image 82fadce3, `legacy/local/physical/82fadce3/serial.log`, read-only)

- The owner pasted an MP3 from the stick onto the desktop: "copia del file rende il sistema instabile fin quando non copia il file (per es. sul desktop) e non appare il dialog che ti mostra lo stato della copia". No `[files] job` record exists in the boot: the copy did not go through Files' f2-84 worker but through the desktop folder (`apps/desktop/folder.c:243–253`, `desk_folder_service`): 24 synchronous `fm_job_step` calls per service turn on the shell's UI thread, 16 KiB chunks (`FM_CHUNK` default), no progress dialog, a replace prompt answered with `FM_CANCEL` and an error box.
- CPU during that copy: `[cpu] top=files#25:495k0s0,desktop#17:259k256s3,…` and `desktop#17:514k364s9` later: the shell's UI thread spent 25–51 % of the CPU (mostly kernel time: the copy's I/O), and the Files window's UI thread (`files#25`, pid 3, which was only showing a folder) 49 % of the CPU in user mode with the window idle. Files' own worker (`files#26`) was at 4–5 %.
- Files polls the media/listing state every 500 ms (`T_MEDIA` → `scan_submit(SCAN_MEDIA)`) and the worker's result every 20 ms (`T_BROWSER` → `scan_poll`); a changed listing (the destination file growing) refreshes and repaints the whole view each time. Something in that path, or in the shell's PING/expose loop toward Files, costs 49 % of a Pentium III while another process copies.

## Required

1. **One transfer engine for the desktop.** The desktop folder's copy/move/delete run on the same worker design as f2-84 (shared code, not a copy: move the worker into `apps/files/transfer.c`/`model.c` or a shared module both programs link, with the 64 KiB steps, byte accounting, EINTR back-off, cancel and cleanup). The shell's UI thread never calls the VFS during a transfer.
2. **The same dialog.** A desktop-folder transfer shows the f2-84 progress dialog (bytes, items, rate, remaining time, Cancel) hosted by the shell; replace prompts are real prompts (Yes / Yes to all / No / Cancel), not an automatic cancel. Delete from the desktop shows the Deleting dialog. Pinned frames for both themes in `build/codex/f2-85-frames/`.
3. **Close deadline.** Files' f2-84 report notes the shell terminates a client two seconds after a close request regardless of a running transfer (`apps/desktop/client.c`, `desk_request_close`/`desk_close_timeouts`). Add a close acknowledgement to the client protocol: a client that answers "closing later" within the deadline gets up to 30 s (then the existing forced close); Files uses it while a transfer or its confirmation is pending; document the protocol change in the shell design note. If this needs a public SDK header change, run `python3 scripts/gen_api_reference.py` and `make CAP= apps`.
4. **Files idle cost.** Reproduce on the host the "another process writes into the displayed folder every 100 ms" scenario and find the 49 %: bound the media/listing refresh to one re-listing per second, repaint only the changed rows, and make sure a size-only change of one entry does not rebuild the whole model. Target: Files UI thread ≤ 5 % CPU while an external copy writes into its folder; prove it with the desktop test harness timing.
5. **Verification.** `make CAP= desktop`, `make CAP= apps`, `python3 -B -m unittest discover -s tests/host/desktop -t .`, `python3 -B -m unittest discover -s tests/host -p 'test_desktop_gate.py'`; add the host cases (desktop folder copy with input during the transfer; replace prompt; cancel; Files idle cost). Report `build/codex/f2-85-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. English UI copy. Keep the Ciuki identity assets untouched.
