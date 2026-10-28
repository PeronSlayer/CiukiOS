# sec-k08b: per-user power timers (lead decision K8-2)

- **Written:** 2026-10-28 15:22 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `medium`. Base: branch `wt/sec-k08` (sec-k08 committed as 9ce83bf0d).
- **Decision K8-2** (`docs/directives/sec-lead-decisions.md`): the standby and hard-disk timers are per-user power scheme settings again, editable by every user (administrator, Standard user, Guest), as in Windows XP. The sealed machine `power.cfg` keeps only the power-button and lid actions, written through call-99 SETTING POWER.

## Scope

- In the Control Panel Power page, enable the standby and hard-disk timer controls for every user, saved in the user's own settings. Keep the button and lid controls administrator-only, saved through the service.
- Make sure the desktop applies the user's timers and the machine's button and lid actions.
- Update the H12/RSET/desktop host tests that assert the disabled state.

## Acceptance

- The changed tests pass.
- The desktop (`python3 -B -m unittest discover -s tests/host/desktop -t .`) and security suites gain no failure against the base.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-k08`.
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run tests sequentially with `nice -n 19`.
- Report to `build/codex/sec-k08b-report.md`.
