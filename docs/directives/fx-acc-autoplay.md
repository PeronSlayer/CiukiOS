# fx-acc-autoplay: acc-t23 item 1, CD AutoPlay never runs on main

- **Written:** 2026-10-29 10:44 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-acc-autoplay`, branch `wt/fx-acc-autoplay` from `main`.
- **Evidence:** image `7c01ad46…` (main `c18eff9cb` plus the wine-upper refresh), case `acc-desktop-cd`. The serial log is `/home/peronslayer/Desktop/CiukiOS/build/test-runs/acc-t23/73db3513/serial.log`, with `result.json` and screens in the same directory.

## Defect

The step `cd-std-autoplay` times out after 120 s waiting for:

```
[user N] [desktop] autoplay probe drive=D: entries=… run_kind=… music=… pictures=… result=0
```

Line 3067 shows the optical stack ready (192 sectors). Line 3073 shows the desktop's "D: Disc inserted" notification. The autoplay worker exists (line 301, `worker role=autoplay`), but no `autoplay probe` line ever appears. The same case passed on 2026-10-27 (image `fbc785e6…`, before the user-security integrations).

## Scope

1. Find why the autoplay worker never probes the inserted disc on main. Candidates:
   - the session and user-security work: the desktop is now launched by LOGON under a user identity, so check the media-change event subscription, the per-user AutoPlay setting and the profile hive defaults;
   - the worker mailbox;
   - the notification path that now runs instead.

   Prove the cause with a host test reproducing it on the production code path.
2. Fix it the Windows 98/XP way. AutoPlay runs in the interactive user's session for the media that the user's settings allow (`NoDriveTypeAutoRun` default: CD-ROM allowed). It still runs after a logon, a Log Off and a new logon.
3. Keep the probe record format the acceptance predicate expects. Do not change the acceptance case.
4. Run the desktop and security host tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds; the lead reruns the case.
- Write the report to `build/codex/fx-acc-autoplay-report.md`.
