# f2-91 — Desktop: merge pointer motion for v3 windows; never drop a busy window's channel

- **Phase/model:** F2, Claude agent (Sonnet). **Worktree:** `wt/f2-91` from `main`.
- **Evidence (f6-20 report):** the desktop sends a v3 client every pointer motion over its window without merging; when 64 messages back up in the desktop plus 64 in the kernel channel, the desktop drops the window (`apps/desktop/client.c:91` and `:158`). A client with a slow first paint (NetSurf on the T23, software rendering) then sees a channel close and exits. Older-protocol windows already get merged motion in `desk_queue`.

## Required

1. Merge consecutive pointer-motion messages for v3 windows in `desk_queue` exactly as for older windows (the last position wins, buttons and keys never merged or reordered).
2. A full client queue never drops the window: the desktop holds the oldest merged motion and keeps the key/button events; it marks the window "busy" (same path as "not responding") and resumes delivery when the client drains; the drop at 128 pending stays only for a client that has not read anything for the not-responding deadline, with a record `[desktop] client_drop window=… pending=… silent_ms=…`.
3. Host tests: a slow client under a motion storm keeps its channel and receives merged motion; keys and buttons preserved in order; the drop record only after the deadline.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f2-91-report.md`.
